/*
  +----------------------------------------------------------------------+
  | php-navicat                                                           |
  +----------------------------------------------------------------------+
  | Copyright (c) Maxime Larrivée-Roy                                     |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2 of the GNU General Public   |
  | License, that is bundled with this package in the file LICENSE.      |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "ext/standard/url.h"
#include "ext/standard/base64.h"
#include "zend_smart_str.h"
#include "php_navicat.h"

#include <curl/curl.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ========================================================================
 * A native client for Navicat's own HTTP-tunnel protocol -- the binary
 * wire format its `ntunnel_mysql.php` (and the pgsql/sqlite siblings it
 * ships alongside it, not yet implemented here) server-side script speaks
 * over a single POST request/response. Reverse-engineered from those
 * scripts directly (they are the only spec that exists) plus the
 * pre-existing PHP-only client this extension replaces the network core
 * of; see docs/PROTOCOL.md and docs/DECISIONS.md.
 *
 * Deliberately minimal/procedural (no PHP class, no custom zend_object):
 * a connection is a plain Zend resource wrapping one reused CURL easy
 * handle plus the connection parameters (which must be resent on every
 * request -- the tunnel itself is stateless, see navicat_build_fields()).
 * ======================================================================== */

/* The wire format is identical between mysql and pgsql (same POST field
 * names, same resultset byte layout) -- confirmed by reading Navicat's own
 * ntunnel_mysql.php/ntunnel_pgsql.php side by side, not assumed. sqlite is
 * genuinely different: it connects via a `dbfile` (no host/port/login/
 * password/db) and, per ntunnel_sqlite.php's EchoData()/EchoData3(), its
 * resultset rows carry an extra 4-byte value-type code after *every* field
 * value (SQLite has per-value dynamic typing, unlike MySQL/Postgres'
 * per-column static typing) -- something the original docs/PROTOCOL.md protocol
 * writeup, drafted before either script had actually been read, assumed
 * away as "identical wire format, only the POST fields differ". See
 * navicat_build_fields()/navicat_read_resultset() for where each backend's
 * real difference is actually handled. */
typedef enum {
	NAVICAT_BACKEND_MYSQL = 0,
	NAVICAT_BACKEND_PGSQL,
	NAVICAT_BACKEND_SQLITE
} navicat_backend;

typedef struct _navicat_connection {
	CURL *curl;
	navicat_backend backend;
	zend_string *url;
	zend_string *host;
	zend_long port;
	zend_string *user;
	zend_string *pass;
	zend_string *db;
	zend_string *dbfile; /* sqlite only */
	zend_string *charset;
	zend_bool use_base64;
	zend_long timeout;
	zend_long conntimeout;
	zend_string *info_host;
	zend_string *info_proto;
	zend_string *info_version;
	zend_string *last_error;
} navicat_connection;

static int le_navicat_connection;
#define LE_NAVICAT_CONNECTION_NAME "Navicat Connection"

static void navicat_release_str(zend_string **s) {
	if (*s) {
		zend_string_release(*s);
		*s = NULL;
	}
}

static void navicat_connection_free(navicat_connection *conn) {
	if (!conn) {
		return;
	}
	if (conn->curl) {
		curl_easy_cleanup(conn->curl);
	}
	navicat_release_str(&conn->url);
	navicat_release_str(&conn->host);
	navicat_release_str(&conn->user);
	navicat_release_str(&conn->pass);
	navicat_release_str(&conn->db);
	navicat_release_str(&conn->dbfile);
	navicat_release_str(&conn->charset);
	navicat_release_str(&conn->info_host);
	navicat_release_str(&conn->info_proto);
	navicat_release_str(&conn->info_version);
	navicat_release_str(&conn->last_error);
	efree(conn);
}

static void navicat_connection_dtor(zend_resource *rsrc) {
	navicat_connection_free((navicat_connection *) rsrc->ptr);
}

static void navicat_set_last_error(navicat_connection *conn, zend_string *msg) {
	navicat_release_str(&conn->last_error);
	conn->last_error = msg;
}

static void navicat_set_last_error_cstr(navicat_connection *conn, const char *msg) {
	navicat_set_last_error(conn, zend_string_init(msg, strlen(msg), 0));
}

/* ------------------------------------------------------------------------
 * smart_str helpers (a smart_str with no data yet has a NULL .s, so every
 * accessor here has to tolerate that instead of dereferencing it)
 * ---------------------------------------------------------------------- */

static size_t navicat_smart_str_len(smart_str *s) {
	return s->s ? ZSTR_LEN(s->s) : 0;
}

static const char *navicat_smart_str_val(smart_str *s) {
	return s->s ? ZSTR_VAL(s->s) : "";
}

/* ------------------------------------------------------------------------
 * POST body construction (application/x-www-form-urlencoded, built by
 * hand -- matches what px.ntunnel.class.php's http_build_query() call
 * sends for actn=Q; the array field is encoded as q[0]=...&q[1]=...).
 * ---------------------------------------------------------------------- */

static void navicat_append_field(smart_str *body, const char *name, const char *value, size_t value_len) {
	zend_string *encoded;
	if (navicat_smart_str_len(body) > 0) {
		smart_str_appendc(body, '&');
	}
	smart_str_appends(body, name);
	smart_str_appendc(body, '=');
	encoded = php_url_encode(value, value_len);
	smart_str_append(body, encoded);
	zend_string_release(encoded);
}

static void navicat_append_field_zstr(smart_str *body, const char *name, zend_string *value) {
	navicat_append_field(body, name, value ? ZSTR_VAL(value) : "", value ? ZSTR_LEN(value) : 0);
}

/* Builds the POST body for one tunnel request. `queries` (may be NULL when
 * `queries_count` is 0) are sent base64-encoded when the connection has
 * base64 mode on (the default -- avoids the tunnel's PHP host having to
 * deal with raw control bytes in $_POST), matching
 * px.ntunnel.class.php::fields(). Field *order* doesn't matter --
 * application/x-www-form-urlencoded is parsed by name on the PHP side --
 * so branching by backend here doesn't need to preserve mysql's original
 * field order for the fields the backends share.
 *
 * sqlite's own `actn` values mean something different from mysql/pgsql's
 * ("C" = test/use an existing file, auto-detecting SQLite2 vs SQLite3 from
 * its header; "2"/"3" = create a *new* database file as SQLite2/SQLite3
 * respectively, per ntunnel_sqlite.php) -- `navicat_sqlite_connect()`
 * passes the right one through its own `actn` argument, same as every
 * other caller of this function. ntunnel_sqlite.php also requires a
 * `version` POST field to be *present* whenever actn is "2"/"3" (checked
 * with `isset()`, never actually read afterwards) -- its value is
 * therefore arbitrary; "1" satisfies the check.
 *
 * `encodeBase64` is always sent as "0" when there are no queries
 * (`queries_count == 0`, i.e. every connect-time request), regardless of
 * `conn->use_base64` -- a real, confirmed bug in Navicat's own current
 * ntunnel_*.php scripts on PHP 8+: they all do
 * `if ($_POST["encodeBase64"] == '1') { for ($i = 0; $i < count($_POST["q"]); ...) }`
 * unconditionally, and `count(null)` (there is no "q" field at all on a
 * connect request) is a fatal TypeError as of PHP 8.0, not just a warning
 * like it used to be. Verified directly against a real, unmodified
 * ntunnel_mysql.php running on PHP 8.4: `encodeBase64=1` with no `q[]`
 * crashes the request outright; `encodeBase64=0` (this fix) does not.
 * Since there is nothing to decode on a connect request either way, "0"
 * here changes no real behavior -- `conn->use_base64` still governs
 * actual query encoding once queries_count > 0. */
static void navicat_build_fields(navicat_connection *conn, const char *actn, zend_string **queries, uint32_t queries_count, smart_str *body) {
	uint32_t i;

	memset(body, 0, sizeof(*body));

	navicat_append_field(body, "actn", actn, strlen(actn));

	if (conn->backend == NAVICAT_BACKEND_SQLITE) {
		navicat_append_field_zstr(body, "dbfile", conn->dbfile);
		if (actn[0] == '2' || actn[0] == '3') {
			navicat_append_field(body, "version", "1", 1);
		}
	} else {
		char portbuf[32];
		int portlen;
		navicat_append_field_zstr(body, "host", conn->host);
		portlen = snprintf(portbuf, sizeof(portbuf), "%ld", (long) conn->port);
		navicat_append_field(body, "port", portbuf, (size_t) portlen);
		navicat_append_field_zstr(body, "login", conn->user);
		navicat_append_field_zstr(body, "password", conn->pass);
		navicat_append_field_zstr(body, "db", conn->db);
	}

	navicat_append_field(body, "encodeBase64", (queries_count > 0 && conn->use_base64) ? "1" : "0", 1);

	for (i = 0; i < queries_count; i++) {
		char name[32];
		int name_len = snprintf(name, sizeof(name), "q[%u]", (unsigned) i);
		(void) name_len;
		if (conn->use_base64) {
			zend_string *b64 = php_base64_encode((const unsigned char *) ZSTR_VAL(queries[i]), ZSTR_LEN(queries[i]));
			navicat_append_field(body, name, ZSTR_VAL(b64), ZSTR_LEN(b64));
			zend_string_release(b64);
		} else {
			navicat_append_field(body, name, ZSTR_VAL(queries[i]), ZSTR_LEN(queries[i]));
		}
	}

	smart_str_0(body);
}

static size_t navicat_curl_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
	smart_str *buf = (smart_str *) userdata;
	size_t total = size * nmemb;
	smart_str_appendl(buf, ptr, total);
	return total;
}

/* Runs one POST request/response cycle. On failure (transport error or a
 * non-200 HTTP status), returns 0 and sets *error_out to a newly-owned
 * zend_string describing what went wrong -- the caller decides whether
 * that's also the connection's last_error. */
static int navicat_perform(navicat_connection *conn, smart_str *body, smart_str *response, zend_string **error_out) {
	CURLcode rc;
	long http_code = 0;

	if (!conn->curl) {
		*error_out = zend_string_init(ZEND_STRL("connection is not initialized"), 0);
		return 0;
	}

	curl_easy_setopt(conn->curl, CURLOPT_POSTFIELDS, navicat_smart_str_val(body));
	curl_easy_setopt(conn->curl, CURLOPT_POSTFIELDSIZE, (long) navicat_smart_str_len(body));
	curl_easy_setopt(conn->curl, CURLOPT_WRITEFUNCTION, navicat_curl_write);
	curl_easy_setopt(conn->curl, CURLOPT_WRITEDATA, response);

	rc = curl_easy_perform(conn->curl);
	if (rc != CURLE_OK) {
		const char *msg = curl_easy_strerror(rc);
		*error_out = zend_string_init(msg, strlen(msg), 0);
		return 0;
	}

	curl_easy_getinfo(conn->curl, CURLINFO_RESPONSE_CODE, &http_code);
	if (http_code != 200) {
		char buf[64];
		int n = snprintf(buf, sizeof(buf), "HTTP error %ld", http_code);
		*error_out = zend_string_init(buf, (size_t) n, 0);
		return 0;
	}

	return 1;
}

/* ------------------------------------------------------------------------
 * Wire format reader.
 *
 * Header (16 bytes): u32 magic (1111) + u16 version + u32 errno + 6 bytes
 * reserved.
 *
 * A "block" is a length-prefixed byte string: 1 byte length (0-253) then
 * that many bytes, OR 0xFE then a u32 length then that many bytes. A
 * length byte of 0 *or* 0xFF both read back as PHP null -- this exact
 * quirk comes from px.ntunnel.class.php's own ntunnel_readblock() (a
 * genuinely empty block and the explicit "value is NULL" marker are
 * indistinguishable there), reproduced here byte-for-byte since that
 * existing reader is what has always talked correctly to Navicat's real
 * server.
 *
 * A resultset is: u32 errno, u32 affectrows, u32 insertid, u32 numfields,
 * u32 numrows, 12 reserved bytes; if errno != 0, one block (the error
 * message) and nothing else; otherwise, if numfields > 0, one
 * (name-block, table-block, u32 type, u32 flags, u32 length) per field
 * followed by numrows * numfields blocks (row-major, NULL-per-value
 * allowed), or, if numfields == 0, a single informational block. A
 * response can carry several resultsets back to back (one per query sent,
 * in order); each is followed by 1 continuation byte (nonzero = more
 * follow).
 * ---------------------------------------------------------------------- */

typedef struct {
	const unsigned char *data;
	size_t len;
	size_t pos;
} navicat_reader;

static int navicat_reader_u8(navicat_reader *r, unsigned char *out) {
	if (r->pos + 1 > r->len) {
		return 0;
	}
	*out = r->data[r->pos++];
	return 1;
}

static int navicat_reader_u16(navicat_reader *r, uint16_t *out) {
	if (r->pos + 2 > r->len) {
		return 0;
	}
	*out = (uint16_t) ((r->data[r->pos] << 8) | r->data[r->pos + 1]);
	r->pos += 2;
	return 1;
}

static int navicat_reader_u32(navicat_reader *r, uint32_t *out) {
	if (r->pos + 4 > r->len) {
		return 0;
	}
	*out = ((uint32_t) r->data[r->pos] << 24) | ((uint32_t) r->data[r->pos + 1] << 16)
		| ((uint32_t) r->data[r->pos + 2] << 8) | (uint32_t) r->data[r->pos + 3];
	r->pos += 4;
	return 1;
}

static int navicat_reader_skip(navicat_reader *r, size_t n) {
	if (r->pos + n > r->len) {
		return 0;
	}
	r->pos += n;
	return 1;
}

/* *out is set to NULL for the protocol's null markers (see the comment
 * above); returns 0 only on truncated/malformed input. */
static int navicat_reader_block(navicat_reader *r, zend_string **out) {
	unsigned char lenb;
	uint32_t len;

	*out = NULL;
	if (!navicat_reader_u8(r, &lenb)) {
		return 0;
	}
	if (lenb == 0 || lenb == 0xFF) {
		return 1;
	}
	if (lenb == 0xFE) {
		if (!navicat_reader_u32(r, &len)) {
			return 0;
		}
	} else {
		len = lenb;
	}
	if (r->pos + len > r->len) {
		return 0;
	}
	*out = zend_string_init((const char *) (r->data + r->pos), len, 0);
	r->pos += len;
	return 1;
}

typedef struct {
	uint16_t version;
	uint32_t errno_;
} navicat_header;

static int navicat_read_header(navicat_reader *r, navicat_header *hdr, zend_string **error_out) {
	uint32_t magic;
	if (!navicat_reader_u32(r, &magic) || magic != 1111) {
		*error_out = zend_string_init(ZEND_STRL("invalid tunnel response (bad magic)"), 0);
		return 0;
	}
	if (!navicat_reader_u16(r, &hdr->version) || !navicat_reader_u32(r, &hdr->errno_) || !navicat_reader_skip(r, 6)) {
		*error_out = zend_string_init(ZEND_STRL("truncated tunnel response"), 0);
		return 0;
	}
	return 1;
}

/* Reads one resultset into a fresh array zval: status, errno, errmsg,
 * affectrows, insertid, numfields, numrows, fields, fieldnames, rows (or
 * just status/errno/info when numfields is 0). Returns 0 on truncated
 * input (the *out array, if partially built, is left for the caller to
 * dtor).
 *
 * `value_types` is only true for the sqlite backend: per the struct
 * comment above navicat_connection, ntunnel_sqlite.php's EchoData()/
 * EchoData3() append one extra u32 value-type code after *every* field
 * value in a row (SQLite's own per-value dynamic typing -- its field
 * *header* carries a static, useless placeholder type instead, e.g.
 * always SQLITE3_NULL for the SQLite3 path). When true, those codes are
 * read and returned as a "valuetypes" array (row-major, parallel to
 * "rows") rather than silently discarded -- for sqlite, the header's
 * "type" is not the real per-value type, so a caller has no other way to
 * get it. */
static int navicat_read_resultset(navicat_reader *r, zval *out, zend_bool value_types) {
	uint32_t rs_errno, affectrows, insertid, numfields, numrows;

	/* Initialized before any read can fail: navicat_read_resultsets() below
	 * always calls zval_ptr_dtor() on `out` when this returns 0 (a
	 * malformed/truncated response, entirely attacker/network-controlled
	 * input), and dtor'ing a still-undefined zval is undefined behavior --
	 * a real crash risk, not hypothetical, found by reading this code
	 * rather than by a fuzzer. */
	array_init(out);

	if (!navicat_reader_u32(r, &rs_errno) || !navicat_reader_u32(r, &affectrows)
		|| !navicat_reader_u32(r, &insertid) || !navicat_reader_u32(r, &numfields)
		|| !navicat_reader_u32(r, &numrows) || !navicat_reader_skip(r, 12)) {
		return 0;
	}
	add_assoc_bool(out, "status", rs_errno == 0);
	add_assoc_long(out, "errno", (zend_long) rs_errno);
	add_assoc_long(out, "affectrows", (zend_long) affectrows);
	add_assoc_long(out, "insertid", (zend_long) insertid);
	add_assoc_long(out, "numfields", (zend_long) numfields);
	add_assoc_long(out, "numrows", (zend_long) numrows);

	if (rs_errno != 0) {
		zend_string *msg = NULL;
		if (!navicat_reader_block(r, &msg)) {
			return 0;
		}
		if (msg) {
			add_assoc_str(out, "errmsg", msg);
		} else {
			add_assoc_null(out, "errmsg");
		}
		return 1;
	}

	if (numfields == 0) {
		zend_string *info = NULL;
		if (!navicat_reader_block(r, &info)) {
			return 0;
		}
		if (info) {
			add_assoc_str(out, "info", info);
		} else {
			add_assoc_null(out, "info");
		}
		return 1;
	}

	{
		zval fields, fieldnames, rows, valuetypes;
		/* Kept alive only long enough to key each row by field name below
		 * (array_combine($fieldnames, $row) in NTunnel_Resultset terms);
		 * released once every row has been built. */
		zend_string **names = safe_emalloc(numfields, sizeof(zend_string *), 0);
		uint32_t i, j;
		int ok = 1;

		for (i = 0; i < numfields; i++) {
			names[i] = NULL;
		}

		array_init(&fields);
		array_init(&fieldnames);
		array_init(&rows);
		if (value_types) {
			array_init(&valuetypes);
		}

		for (i = 0; ok && i < numfields; i++) {
			zend_string *field_name = NULL, *table_name = NULL;
			uint32_t type, flag, length;

			if (!navicat_reader_block(r, &field_name) || !navicat_reader_block(r, &table_name)
				|| !navicat_reader_u32(r, &type) || !navicat_reader_u32(r, &flag)
				|| !navicat_reader_u32(r, &length)) {
				navicat_release_str(&field_name);
				navicat_release_str(&table_name);
				ok = 0;
				break;
			}

			if (!field_name) {
				field_name = ZSTR_EMPTY_ALLOC();
			}
			if (!table_name) {
				table_name = ZSTR_EMPTY_ALLOC();
			}
			names[i] = zend_string_copy(field_name);

			{
				zval field_info;
				array_init(&field_info);
				add_assoc_str(&field_info, "name", field_name);
				add_assoc_str(&field_info, "table", table_name);
				add_assoc_long(&field_info, "type", (zend_long) type);
				add_assoc_long(&field_info, "flags", (zend_long) flag);
				add_assoc_long(&field_info, "length", (zend_long) length);
				add_next_index_zval(&fields, &field_info);
			}
			add_next_index_str(&fieldnames, zend_string_copy(names[i]));
		}

		for (i = 0; ok && i < numrows; i++) {
			zval row, rowtypes;
			array_init(&row);
			if (value_types) {
				array_init(&rowtypes);
			}
			for (j = 0; j < numfields; j++) {
				zend_string *val = NULL;
				zval v;
				if (!navicat_reader_block(r, &val)) {
					ok = 0;
					break;
				}
				if (val) {
					ZVAL_STR(&v, val);
				} else {
					ZVAL_NULL(&v);
				}
				zend_hash_update(Z_ARRVAL(row), names[j], &v);

				if (value_types) {
					uint32_t vtype;
					if (!navicat_reader_u32(r, &vtype)) {
						ok = 0;
						break;
					}
					add_next_index_long(&rowtypes, (zend_long) vtype);
				}
			}
			if (!ok) {
				zval_ptr_dtor(&row);
				if (value_types) {
					zval_ptr_dtor(&rowtypes);
				}
				break;
			}
			add_next_index_zval(&rows, &row);
			if (value_types) {
				add_next_index_zval(&valuetypes, &rowtypes);
			}
		}

		for (i = 0; i < numfields; i++) {
			navicat_release_str(&names[i]);
		}
		efree(names);

		if (!ok) {
			zval_ptr_dtor(&fields);
			zval_ptr_dtor(&fieldnames);
			zval_ptr_dtor(&rows);
			if (value_types) {
				zval_ptr_dtor(&valuetypes);
			}
			return 0;
		}

		add_assoc_zval(out, "fields", &fields);
		add_assoc_zval(out, "fieldnames", &fieldnames);
		add_assoc_zval(out, "rows", &rows);
		if (value_types) {
			add_assoc_zval(out, "valuetypes", &valuetypes);
		}
	}

	return 1;
}

/* Reads every resultset in the response (one per query the request sent,
 * in order) into a numerically-indexed array. */
static int navicat_read_resultsets(navicat_reader *r, zval *out, zend_bool value_types) {
	array_init(out);
	while (1) {
		zval rs;
		unsigned char more;
		if (!navicat_read_resultset(r, &rs, value_types)) {
			zval_ptr_dtor(&rs);
			return 0;
		}
		add_next_index_zval(out, &rs);
		if (!navicat_reader_u8(r, &more) || !more) {
			break;
		}
	}
	return 1;
}

/* ------------------------------------------------------------------------
 * PHP-visible functions
 * ---------------------------------------------------------------------- */

ZEND_BEGIN_ARG_INFO_EX(arginfo_navicat_connect, 0, 0, 5)
	ZEND_ARG_INFO(0, url)
	ZEND_ARG_INFO(0, host)
	ZEND_ARG_INFO(0, port)
	ZEND_ARG_INFO(0, user)
	ZEND_ARG_INFO(0, password)
	ZEND_ARG_INFO(0, db)
	ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()

/* Shared by navicat_connect() (mysql) and navicat_pg_connect() (pgsql) --
 * ntunnel_mysql.php and ntunnel_pgsql.php take the exact same POST fields
 * and reply with the exact same 3-block actn=C response (confirmed by
 * reading both directly, not assumed), so the only thing that actually
 * differs between the two backends is which tunnel script's URL the
 * caller happens to point $url at; navicat_build_fields() already branches
 * on conn->backend for the (identical, here) mysql/pgsql field set, so no
 * further backend-specific logic is needed in this function itself. */
static void navicat_do_connect_hostbased(INTERNAL_FUNCTION_PARAMETERS, navicat_backend backend, const char *fname)
{
	char *url, *host, *user, *pass, *db = NULL;
	size_t url_len, host_len, user_len, pass_len, db_len = 0;
	zend_long port;
	zval *options = NULL;
	navicat_connection *conn;
	const char *proxy = NULL;
	smart_str body = {0}, response = {0};
	zend_string *error = NULL;
	navicat_reader reader;
	navicat_header hdr;

	ZEND_PARSE_PARAMETERS_START(5, 7)
		Z_PARAM_STRING(url, url_len)
		Z_PARAM_STRING(host, host_len)
		Z_PARAM_LONG(port)
		Z_PARAM_STRING(user, user_len)
		Z_PARAM_STRING(pass, pass_len)
		Z_PARAM_OPTIONAL
		Z_PARAM_STRING(db, db_len)
		Z_PARAM_ARRAY(options)
	ZEND_PARSE_PARAMETERS_END();

	conn = ecalloc(1, sizeof(navicat_connection));
	conn->backend = backend;
	conn->url = zend_string_init(url, url_len, 0);
	conn->host = zend_string_init(host, host_len, 0);
	conn->port = port;
	conn->user = zend_string_init(user, user_len, 0);
	conn->pass = zend_string_init(pass, pass_len, 0);
	conn->db = zend_string_init(db ? db : "", db_len, 0);
	conn->charset = zend_string_init(ZEND_STRL("utf8"), 0);
	conn->use_base64 = 1;
	conn->timeout = 600;
	conn->conntimeout = 30;

	if (options) {
		zval *tmp;
		if ((tmp = zend_hash_str_find(Z_ARRVAL_P(options), ZEND_STRL("charset"))) != NULL && Z_TYPE_P(tmp) == IS_STRING) {
			navicat_release_str(&conn->charset);
			conn->charset = zend_string_copy(Z_STR_P(tmp));
		}
		if ((tmp = zend_hash_str_find(Z_ARRVAL_P(options), ZEND_STRL("base64"))) != NULL) {
			conn->use_base64 = zend_is_true(tmp);
		}
		if ((tmp = zend_hash_str_find(Z_ARRVAL_P(options), ZEND_STRL("timeout"))) != NULL) {
			conn->timeout = zval_get_long(tmp);
		}
		if ((tmp = zend_hash_str_find(Z_ARRVAL_P(options), ZEND_STRL("conntimeout"))) != NULL) {
			conn->conntimeout = zval_get_long(tmp);
		}
		if ((tmp = zend_hash_str_find(Z_ARRVAL_P(options), ZEND_STRL("proxy"))) != NULL && Z_TYPE_P(tmp) == IS_STRING) {
			proxy = Z_STRVAL_P(tmp);
		}
	}

	conn->curl = curl_easy_init();
	if (!conn->curl) {
		php_error_docref(NULL, E_WARNING, "%s(): failed to initialize curl", fname);
		navicat_connection_free(conn);
		RETURN_FALSE;
	}

	curl_easy_setopt(conn->curl, CURLOPT_URL, ZSTR_VAL(conn->url));
	curl_easy_setopt(conn->curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(conn->curl, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(conn->curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(conn->curl, CURLOPT_POST, 1L);
	curl_easy_setopt(conn->curl, CURLOPT_TIMEOUT, (long) conn->timeout);
	curl_easy_setopt(conn->curl, CURLOPT_CONNECTTIMEOUT, (long) conn->conntimeout);
	curl_easy_setopt(conn->curl, CURLOPT_USERAGENT, "Navicat HTTP Tunnel");
	if (proxy) {
		curl_easy_setopt(conn->curl, CURLOPT_PROXY, proxy);
	}

	navicat_build_fields(conn, "C", NULL, 0, &body);
	if (!navicat_perform(conn, &body, &response, &error)) {
		smart_str_free(&body);
		smart_str_free(&response);
		php_error_docref(NULL, E_WARNING, "%s(): %s", fname, error ? ZSTR_VAL(error) : "unknown error");
		navicat_release_str(&error);
		navicat_connection_free(conn);
		RETURN_FALSE;
	}
	smart_str_free(&body);

	reader.data = (const unsigned char *) navicat_smart_str_val(&response);
	reader.len = navicat_smart_str_len(&response);
	reader.pos = 0;

	if (!navicat_read_header(&reader, &hdr, &error)) {
		smart_str_free(&response);
		php_error_docref(NULL, E_WARNING, "%s(): %s", fname, error ? ZSTR_VAL(error) : "malformed response");
		navicat_release_str(&error);
		navicat_connection_free(conn);
		RETURN_FALSE;
	}

	if (hdr.errno_ != 0) {
		zend_string *msg = NULL;
		navicat_reader_block(&reader, &msg);
		smart_str_free(&response);
		php_error_docref(NULL, E_WARNING, "%s(): tunnel error %u: %s", fname, (unsigned) hdr.errno_, msg ? ZSTR_VAL(msg) : "");
		navicat_release_str(&msg);
		navicat_connection_free(conn);
		RETURN_FALSE;
	}

	navicat_reader_block(&reader, &conn->info_host);
	navicat_reader_block(&reader, &conn->info_proto);
	navicat_reader_block(&reader, &conn->info_version);
	smart_str_free(&response);

	RETURN_RES(zend_register_resource(conn, le_navicat_connection));
}

PHP_FUNCTION(navicat_connect)
{
	navicat_do_connect_hostbased(INTERNAL_FUNCTION_PARAM_PASSTHRU, NAVICAT_BACKEND_MYSQL, "navicat_connect");
}

PHP_FUNCTION(navicat_pg_connect)
{
	navicat_do_connect_hostbased(INTERNAL_FUNCTION_PARAM_PASSTHRU, NAVICAT_BACKEND_PGSQL, "navicat_pg_connect");
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_navicat_sqlite_connect, 0, 0, 2)
	ZEND_ARG_INFO(0, url)
	ZEND_ARG_INFO(0, dbfile)
	ZEND_ARG_INFO(0, options)
ZEND_END_ARG_INFO()

/* ntunnel_sqlite.php's own actn=C tests/uses an *existing* $dbfile,
 * auto-detecting SQLite2 vs SQLite3 from the file's own header bytes --
 * the client never needs to say which one it expects. Creating a *new*
 * database file instead needs an explicit actn of "2" or "3" (there is no
 * "auto" option when the file doesn't exist yet, since there is nothing to
 * sniff), selected here via $options['create'] = 'sqlite2'|'sqlite3'. */
PHP_FUNCTION(navicat_sqlite_connect)
{
	char *url, *dbfile;
	size_t url_len, dbfile_len;
	zval *options = NULL;
	navicat_connection *conn;
	const char *proxy = NULL;
	const char *actn = "C";
	smart_str body = {0}, response = {0};
	zend_string *error = NULL;
	navicat_reader reader;
	navicat_header hdr;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_STRING(url, url_len)
		Z_PARAM_STRING(dbfile, dbfile_len)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY(options)
	ZEND_PARSE_PARAMETERS_END();

	conn = ecalloc(1, sizeof(navicat_connection));
	conn->backend = NAVICAT_BACKEND_SQLITE;
	conn->url = zend_string_init(url, url_len, 0);
	conn->dbfile = zend_string_init(dbfile, dbfile_len, 0);
	conn->charset = zend_string_init(ZEND_STRL("utf8"), 0);
	conn->use_base64 = 1;
	conn->timeout = 600;
	conn->conntimeout = 30;

	if (options) {
		zval *tmp;
		if ((tmp = zend_hash_str_find(Z_ARRVAL_P(options), ZEND_STRL("create"))) != NULL && Z_TYPE_P(tmp) == IS_STRING) {
			if (zend_string_equals_literal(Z_STR_P(tmp), "sqlite2")) {
				actn = "2";
			} else if (zend_string_equals_literal(Z_STR_P(tmp), "sqlite3")) {
				actn = "3";
			} else {
				php_error_docref(NULL, E_WARNING,
					"navicat_sqlite_connect(): options['create'] must be 'sqlite2' or 'sqlite3'");
				navicat_connection_free(conn);
				RETURN_FALSE;
			}
		}
		if ((tmp = zend_hash_str_find(Z_ARRVAL_P(options), ZEND_STRL("base64"))) != NULL) {
			conn->use_base64 = zend_is_true(tmp);
		}
		if ((tmp = zend_hash_str_find(Z_ARRVAL_P(options), ZEND_STRL("timeout"))) != NULL) {
			conn->timeout = zval_get_long(tmp);
		}
		if ((tmp = zend_hash_str_find(Z_ARRVAL_P(options), ZEND_STRL("conntimeout"))) != NULL) {
			conn->conntimeout = zval_get_long(tmp);
		}
		if ((tmp = zend_hash_str_find(Z_ARRVAL_P(options), ZEND_STRL("proxy"))) != NULL && Z_TYPE_P(tmp) == IS_STRING) {
			proxy = Z_STRVAL_P(tmp);
		}
	}

	conn->curl = curl_easy_init();
	if (!conn->curl) {
		php_error_docref(NULL, E_WARNING, "navicat_sqlite_connect(): failed to initialize curl");
		navicat_connection_free(conn);
		RETURN_FALSE;
	}

	curl_easy_setopt(conn->curl, CURLOPT_URL, ZSTR_VAL(conn->url));
	curl_easy_setopt(conn->curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(conn->curl, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(conn->curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(conn->curl, CURLOPT_POST, 1L);
	curl_easy_setopt(conn->curl, CURLOPT_TIMEOUT, (long) conn->timeout);
	curl_easy_setopt(conn->curl, CURLOPT_CONNECTTIMEOUT, (long) conn->conntimeout);
	curl_easy_setopt(conn->curl, CURLOPT_USERAGENT, "Navicat HTTP Tunnel");
	if (proxy) {
		curl_easy_setopt(conn->curl, CURLOPT_PROXY, proxy);
	}

	navicat_build_fields(conn, actn, NULL, 0, &body);
	if (!navicat_perform(conn, &body, &response, &error)) {
		smart_str_free(&body);
		smart_str_free(&response);
		php_error_docref(NULL, E_WARNING, "navicat_sqlite_connect(): %s", error ? ZSTR_VAL(error) : "unknown error");
		navicat_release_str(&error);
		navicat_connection_free(conn);
		RETURN_FALSE;
	}
	smart_str_free(&body);

	reader.data = (const unsigned char *) navicat_smart_str_val(&response);
	reader.len = navicat_smart_str_len(&response);
	reader.pos = 0;

	if (!navicat_read_header(&reader, &hdr, &error)) {
		smart_str_free(&response);
		php_error_docref(NULL, E_WARNING, "navicat_sqlite_connect(): %s", error ? ZSTR_VAL(error) : "malformed response");
		navicat_release_str(&error);
		navicat_connection_free(conn);
		RETURN_FALSE;
	}

	if (hdr.errno_ != 0) {
		zend_string *msg = NULL;
		navicat_reader_block(&reader, &msg);
		smart_str_free(&response);
		php_error_docref(NULL, E_WARNING, "navicat_sqlite_connect(): tunnel error %u: %s", (unsigned) hdr.errno_, msg ? ZSTR_VAL(msg) : "");
		navicat_release_str(&msg);
		navicat_connection_free(conn);
		RETURN_FALSE;
	}

	/* ntunnel_sqlite.php's EchoConnInfo()/EchoConnInfo3() send the same
	 * version string 3 times (there's no separate host/protocol concept
	 * for an embedded database) -- reusing the mysql/pgsql 3-block layout
	 * here is accurate to the real protocol, not a simplification. */
	navicat_reader_block(&reader, &conn->info_host);
	navicat_reader_block(&reader, &conn->info_proto);
	navicat_reader_block(&reader, &conn->info_version);
	smart_str_free(&response);

	RETURN_RES(zend_register_resource(conn, le_navicat_connection));
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_navicat_connection_info, 0, 0, 1)
	ZEND_ARG_INFO(0, connection)
ZEND_END_ARG_INFO()

PHP_FUNCTION(navicat_connection_info)
{
	zval *zres;
	navicat_connection *conn;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_RESOURCE(zres)
	ZEND_PARSE_PARAMETERS_END();

	conn = zend_fetch_resource(Z_RES_P(zres), LE_NAVICAT_CONNECTION_NAME, le_navicat_connection);
	if (!conn) {
		RETURN_FALSE;
	}

	array_init(return_value);
	add_assoc_str(return_value, "host", conn->info_host ? zend_string_copy(conn->info_host) : ZSTR_EMPTY_ALLOC());
	add_assoc_str(return_value, "proto", conn->info_proto ? zend_string_copy(conn->info_proto) : ZSTR_EMPTY_ALLOC());
	add_assoc_str(return_value, "version", conn->info_version ? zend_string_copy(conn->info_version) : ZSTR_EMPTY_ALLOC());
}

/* Shared by navicat_query()/navicat_multi_query(): sends `count` queries
 * (the caller decides whether to prepend a "SET NAMES ..." charset query,
 * matching px.ntunnel.class.php's own query()/multiquery() for mysql/pgsql
 * -- sqlite has no such statement and skips it, see navicat_query()), and
 * either returns the parsed resultsets array or leaves the connection's
 * last_error set and returns 0. */
static int navicat_run_queries(navicat_connection *conn, zend_string **queries, uint32_t count, zval *out_sets) {
	smart_str body = {0}, response = {0};
	zend_string *error = NULL;
	navicat_reader reader;
	navicat_header hdr;

	navicat_build_fields(conn, "Q", queries, count, &body);
	if (!navicat_perform(conn, &body, &response, &error)) {
		smart_str_free(&body);
		smart_str_free(&response);
		navicat_set_last_error(conn, error);
		return 0;
	}
	smart_str_free(&body);

	reader.data = (const unsigned char *) navicat_smart_str_val(&response);
	reader.len = navicat_smart_str_len(&response);
	reader.pos = 0;

	if (!navicat_read_header(&reader, &hdr, &error)) {
		smart_str_free(&response);
		navicat_set_last_error(conn, error);
		return 0;
	}

	if (hdr.errno_ != 0) {
		zend_string *msg = NULL;
		navicat_reader_block(&reader, &msg);
		smart_str_free(&response);
		navicat_set_last_error(conn, msg ? msg : zend_string_init(ZEND_STRL("tunnel error"), 0));
		return 0;
	}

	if (!navicat_read_resultsets(&reader, out_sets, conn->backend == NAVICAT_BACKEND_SQLITE)) {
		zval_ptr_dtor(out_sets);
		smart_str_free(&response);
		navicat_set_last_error(conn, zend_string_init(ZEND_STRL("malformed resultset in tunnel response"), 0));
		return 0;
	}

	smart_str_free(&response);
	return 1;
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_navicat_query, 0, 0, 2)
	ZEND_ARG_INFO(0, connection)
	ZEND_ARG_INFO(0, query)
ZEND_END_ARG_INFO()

PHP_FUNCTION(navicat_query)
{
	zval *zres;
	char *query;
	size_t query_len;
	navicat_connection *conn;
	zend_string *queries[2];
	uint32_t count;
	zend_bool inject_charset;
	zval sets;
	zval *real;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_RESOURCE(zres)
		Z_PARAM_STRING(query, query_len)
	ZEND_PARSE_PARAMETERS_END();

	conn = zend_fetch_resource(Z_RES_P(zres), LE_NAVICAT_CONNECTION_NAME, le_navicat_connection);
	if (!conn) {
		RETURN_FALSE;
	}

	/* sqlite has no "SET NAMES"/connection-charset concept at all -- the
	 * tunnel is always UTF-8 there -- and ntunnel_sqlite.php has no
	 * equivalent statement to run, so injecting one would just waste a
	 * round-trip on a query that always errors out. */
	inject_charset = (conn->backend != NAVICAT_BACKEND_SQLITE);
	if (inject_charset) {
		queries[0] = strpprintf(0, "SET NAMES '%s'", ZSTR_VAL(conn->charset));
		queries[1] = zend_string_init(query, query_len, 0);
		count = 2;
	} else {
		queries[0] = zend_string_init(query, query_len, 0);
		count = 1;
	}

	if (!navicat_run_queries(conn, queries, count, &sets)) {
		zend_string_release(queries[0]);
		if (inject_charset) {
			zend_string_release(queries[1]);
		}
		RETURN_FALSE;
	}
	zend_string_release(queries[0]);
	if (inject_charset) {
		zend_string_release(queries[1]);
	}

	/* When a "SET NAMES ..." was injected, sets[0] is its own (discarded)
	 * resultset and sets[1] is the real query's -- matching
	 * NTunnel::query()'s array_pop($sets). Without it (sqlite), the real
	 * query is sets[0] directly. */
	real = zend_hash_index_find(Z_ARRVAL(sets), inject_charset ? 1 : 0);
	if (!real) {
		zval_ptr_dtor(&sets);
		RETURN_FALSE;
	}
	ZVAL_COPY(return_value, real);
	zval_ptr_dtor(&sets);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_navicat_multi_query, 0, 0, 2)
	ZEND_ARG_INFO(0, connection)
	ZEND_ARG_INFO(0, queries)
ZEND_END_ARG_INFO()

PHP_FUNCTION(navicat_multi_query)
{
	zval *zres, *queries_arr, *val;
	navicat_connection *conn;
	uint32_t n, i, total;
	zend_bool inject_charset;
	zend_string **queries;
	zval sets, *item;
	uint32_t idx;

	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_RESOURCE(zres)
		Z_PARAM_ARRAY(queries_arr)
	ZEND_PARSE_PARAMETERS_END();

	conn = zend_fetch_resource(Z_RES_P(zres), LE_NAVICAT_CONNECTION_NAME, le_navicat_connection);
	if (!conn) {
		RETURN_FALSE;
	}

	n = zend_hash_num_elements(Z_ARRVAL_P(queries_arr));
	if (n == 0) {
		array_init(return_value);
		return;
	}

	/* See navicat_query() for why sqlite gets no injected "SET NAMES ...". */
	inject_charset = (conn->backend != NAVICAT_BACKEND_SQLITE);
	total = inject_charset ? n + 1 : n;
	queries = safe_emalloc(total, sizeof(zend_string *), 0);
	i = 0;
	if (inject_charset) {
		queries[0] = strpprintf(0, "SET NAMES '%s'", ZSTR_VAL(conn->charset));
		i = 1;
	}
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(queries_arr), val) {
		queries[i++] = zval_get_string(val);
	} ZEND_HASH_FOREACH_END();

	if (!navicat_run_queries(conn, queries, total, &sets)) {
		for (i = 0; i < total; i++) {
			zend_string_release(queries[i]);
		}
		efree(queries);
		RETURN_FALSE;
	}
	for (i = 0; i < total; i++) {
		zend_string_release(queries[i]);
	}
	efree(queries);

	/* When a "SET NAMES ..." was injected, drop index 0 and reindex,
	 * matching NTunnel::multiquery()'s array_shift($sets). Without it
	 * (sqlite), every resultset is a real one -- keep them all. */
	array_init(return_value);
	idx = 0;
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL(sets), item) {
		if (!inject_charset || idx > 0) {
			Z_TRY_ADDREF_P(item);
			add_next_index_zval(return_value, item);
		}
		idx++;
	} ZEND_HASH_FOREACH_END();
	zval_ptr_dtor(&sets);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_navicat_last_error, 0, 0, 1)
	ZEND_ARG_INFO(0, connection)
ZEND_END_ARG_INFO()

PHP_FUNCTION(navicat_last_error)
{
	zval *zres;
	navicat_connection *conn;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_RESOURCE(zres)
	ZEND_PARSE_PARAMETERS_END();

	conn = zend_fetch_resource(Z_RES_P(zres), LE_NAVICAT_CONNECTION_NAME, le_navicat_connection);
	if (!conn) {
		RETURN_FALSE;
	}

	if (conn->last_error) {
		RETURN_STR(zend_string_copy(conn->last_error));
	}
	RETURN_NULL();
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_navicat_close, 0, 0, 1)
	ZEND_ARG_INFO(0, connection)
ZEND_END_ARG_INFO()

PHP_FUNCTION(navicat_close)
{
	zval *zres;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_RESOURCE(zres)
	ZEND_PARSE_PARAMETERS_END();

	zend_list_close(Z_RES_P(zres));
}

/* Direct port of NTunnel::escape() -- same 7-pair str_replace table,
 * applied recursively over arrays (preserving keys), left as-is for
 * non-string/non-array/empty-string values. Needs no curl handle, so it
 * works even without a live connection. */
static zend_string *navicat_escape_string(zend_string *in) {
	smart_str out = {0};
	size_t i;
	for (i = 0; i < ZSTR_LEN(in); i++) {
		unsigned char c = (unsigned char) ZSTR_VAL(in)[i];
		switch (c) {
			case '\\': smart_str_appendl(&out, "\\\\", 2); break;
			case '\0': smart_str_appendl(&out, "\\0", 2); break;
			case '\n': smart_str_appendl(&out, "\\n", 2); break;
			case '\r': smart_str_appendl(&out, "\\r", 2); break;
			case '\'': smart_str_appendl(&out, "\\'", 2); break;
			case '"': smart_str_appendl(&out, "\\\"", 2); break;
			case '\x1a': smart_str_appendl(&out, "\\Z", 2); break;
			default: smart_str_appendc(&out, (char) c); break;
		}
	}
	if (!out.s) {
		return ZSTR_EMPTY_ALLOC();
	}
	smart_str_0(&out);
	return out.s;
}

/* Recurses into nested arrays -- found missing by an actual end-to-end
 * test (a nested array's own string values came back unescaped, byte-
 * for-byte unchanged, despite this function's doc comment promising
 * recursive behavior matching NTunnel::escape()). `result` is an
 * uninitialized zval the caller owns; this always assigns it exactly
 * once (a plain value, a freshly escaped string, or a freshly built
 * array), mirroring the discipline PHP_FUNCTION(navicat_escape) itself
 * used to follow only at its own top level. */
static void navicat_escape_value(zval *value, zval *result) {
	if (Z_TYPE_P(value) == IS_ARRAY) {
		zend_string *key;
		zend_ulong idx;
		zval *entry;

		array_init(result);
		ZEND_HASH_FOREACH_KEY_VAL(Z_ARRVAL_P(value), idx, key, entry) {
			zval escaped;
			navicat_escape_value(entry, &escaped);
			if (key) {
				zend_hash_update(Z_ARRVAL_P(result), key, &escaped);
			} else {
				zend_hash_index_update(Z_ARRVAL_P(result), idx, &escaped);
			}
		} ZEND_HASH_FOREACH_END();
		return;
	}

	if (Z_TYPE_P(value) == IS_STRING && Z_STRLEN_P(value) > 0) {
		ZVAL_STR(result, navicat_escape_string(Z_STR_P(value)));
		return;
	}

	ZVAL_COPY(result, value);
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_navicat_escape, 0, 0, 1)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

PHP_FUNCTION(navicat_escape)
{
	zval *value;

	ZEND_PARSE_PARAMETERS_START(1, 1)
		Z_PARAM_ZVAL(value)
	ZEND_PARSE_PARAMETERS_END();

	navicat_escape_value(value, return_value);
}

/* ------------------------------------------------------------------------
 * Module plumbing
 * ---------------------------------------------------------------------- */

static const zend_function_entry navicat_functions[] = {
	PHP_FE(navicat_connect, arginfo_navicat_connect)
	PHP_FE(navicat_pg_connect, arginfo_navicat_connect)
	PHP_FE(navicat_sqlite_connect, arginfo_navicat_sqlite_connect)
	PHP_FE(navicat_connection_info, arginfo_navicat_connection_info)
	PHP_FE(navicat_query, arginfo_navicat_query)
	PHP_FE(navicat_multi_query, arginfo_navicat_multi_query)
	PHP_FE(navicat_escape, arginfo_navicat_escape)
	PHP_FE(navicat_last_error, arginfo_navicat_last_error)
	PHP_FE(navicat_close, arginfo_navicat_close)
	PHP_FE_END
};

PHP_MINIT_FUNCTION(navicat)
{
	le_navicat_connection = zend_register_list_destructors_ex(navicat_connection_dtor, NULL, LE_NAVICAT_CONNECTION_NAME, module_number);
	curl_global_init(CURL_GLOBAL_DEFAULT);
	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(navicat)
{
	curl_global_cleanup();
	return SUCCESS;
}

PHP_MINFO_FUNCTION(navicat)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "navicat support", "enabled");
	php_info_print_table_row(2, "version", PHP_NAVICAT_VERSION);
	php_info_print_table_row(2, "navicat backends", "mysql, pgsql, sqlite");
	php_info_print_table_row(2, "libcurl version", curl_version());
	/* php_info_print_table_row()/_header() don't escape their arguments,
	 * so a raw two-cell footer row works here too -- same Kirigami logo
	 * used as the README header, kept as a hosted URL rather than a
	 * base64 blob to avoid bloating this binary. Plain <img> also
	 * survives an HTML->Markdown conversion of phpinfo()'s output cleanly
	 * (renders as `![...](url)`), unlike inline <svg> markup, which not
	 * every such converter preserves. */
	php_printf(
		"<tr><td style=\"text-align: left\">Part of the Kirigami PHP extension family</td>"
		"<td style=\"text-align: right\">"
		"<img src=\"https://zmotrin.github.io/assets/kirigami/kirigami-logo-universal.svg\" "
		"alt=\"Kirigami\" height=\"28\" /></td></tr>\n"
	);
	php_info_print_table_end();
}

zend_module_entry navicat_module_entry = {
	STANDARD_MODULE_HEADER,
	"navicat",
	navicat_functions,
	PHP_MINIT(navicat),
	PHP_MSHUTDOWN(navicat),
	NULL,
	NULL,
	PHP_MINFO(navicat),
	PHP_NAVICAT_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_NAVICAT
ZEND_GET_MODULE(navicat)
#endif
