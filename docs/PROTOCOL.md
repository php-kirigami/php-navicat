# Protocol reference

Reverse-engineered directly from Navicat's own server scripts —
`ntunnel_mysql.php`/`ntunnel_pgsql.php`/`ntunnel_sqlite.php`, read directly
from a real `C:\Program Files\PremiumSoft\Navicat Premium Lite
17\resource\httptunnel\` install, 2026-09-16.

The header/block/resultset-header framing below is **shared by all three
backends**. What differs — corrected here after actually reading
`ntunnel_pgsql.php`/`ntunnel_sqlite.php` side by side with the mysql one
(the original version of this section, written before either file had
been read, guessed "only the POST fields and the per-column type-code
mapping differ" — true for pgsql, **not** true for sqlite, see below):

- **mysql vs. pgsql: only the tunnel URL differs, not the wire format.**
  `ntunnel_pgsql.php` takes the *exact same* POST fields as
  `ntunnel_mysql.php` (`actn`, `host`, `port`, `login`, `password`, `db`,
  `encodeBase64`, `q[]`) and replies with the exact same 16-byte
  header + 3-block `actn=C` response + resultset framing. Its own libpq
  connection string is built entirely server-side from those same fields
  (`"host=... port=... dbname='...' user=... password=..."`) — the client
  never sees or sends it. The only real differences are semantic, not
  wire-level: `insertid` is always `0` (Postgres has no MySQL-style
  auto-increment id concept the tunnel surfaces), and any query error
  collapses to a generic `errno=1` (`pg_last_error($conn) <> ""`) rather
  than a real driver error code.
- **sqlite is genuinely different, at the byte level, not just in which
  POST fields it takes.** It connects via `dbfile` (a server-side path,
  no `host`/`port`/`login`/`password`/`db` at all) and `actn` means
  something else entirely: `C` = test/use an *existing* file (the server
  auto-detects SQLite2 vs SQLite3 from the file's own header bytes —
  `"** This file contains an SQLite 2.1 database **"` vs. `"SQLite
  format 3"` — the client never has to say which one it expects), while
  `2`/`3` *create a new* database file as SQLite2/SQLite3 respectively
  (only valid when the file doesn't already exist) and additionally
  require a `version` POST field to merely be *present* (`isset()`
  checked, its value is never actually read anywhere in the script — any
  value satisfies it). `EchoConnInfo()`/`EchoConnInfo3()` send the *same*
  version string 3 times (no separate host/protocol concept for an
  embedded database). **The real wire-format difference**: SQLite's own
  per-value dynamic typing means `EchoData()`/`EchoData3()` append one
  *extra* 4-byte value-type code after **every** field value in every row
  (`GetLongBinary(-2)` — the `SQLITE_TEXT` constant, hardcoded — for the
  legacy SQLite2 path; `GetLongBinary($res->columnType($j))` — the real,
  per-value SQLite3 type constant — for SQLite3), which mysql/pgsql never
  send at all (their field *header*, sent once per column, is the only
  place a type code appears). Correspondingly, sqlite's field *header*
  carries a static, useless type placeholder (`-2` always for SQLite2;
  `SQLITE3_NULL` always for SQLite3) — a caller that actually wants a
  sqlite result's real per-value type has to read it from the row data,
  not the header.

This repo implements all three backends now (see "Decided architecture"
below) — the MySQL-only scope note that used to be here (and the
now-corrected "identical wire format" claim above it) reflected an
earlier session that hadn't looked at the pgsql/sqlite scripts yet.

**Request**: a single `POST` to the tunnel script's URL,
`application/x-www-form-urlencoded`, always including `actn` (`C` = test
connection, `Q` = run queries) and `encodeBase64` (`0`/`1`). For MySQL:
`host`, `port`, `login`, `password`, `db`. For `actn=Q`: `q[0]`, `q[1]`,
... (one per query, base64-encoded when `encodeBase64=1`). The tunnel is
**stateless** — every request resends the full connection parameters,
there is no server-side session.

**Response**: raw binary (`Content-Type: text/plain; charset=x-user-defined`
on the server side, so no charset translation mangles it), all integers
big-endian:

- **Header** (16 bytes): `u32` magic (`1111`) + `u16` protocol version +
  `u32` errno + 6 reserved bytes. A nonzero errno here means the whole
  request failed (bad params, unsupported PHP version, DB connect
  failure, ...); one **block** (see below) follows with the error message,
  and that's the entire response.
- **Block** (variable-length, length-prefixed byte string): 1 byte length
  (`0`-`253`) then that many raw bytes, *or* `0xFE` then a `u32` length
  then that many bytes. A length byte of `0` **or** `0xFF` both read back
  as `NULL` in the existing client's reader
  (`px.ntunnel.class.php::ntunnel_readblock()`) — a genuinely empty
  string and the explicit "value is NULL" marker are indistinguishable
  there. This extension reproduces that exact quirk byte-for-byte, since
  that reader is the thing that has always talked correctly to Navicat's
  real server (i.e. it's not a bug to fix, it's the de facto spec).
- **`actn=C` success** (errno `0`): 3 blocks — host info, protocol info,
  server version string.
- **`actn=Q` success** (errno `0`): one **resultset** per query sent, back
  to back, each followed by **1 continuation byte** (nonzero = more
  resultsets follow, `0` = last one). A resultset is:
  - `u32` errno, `u32` affectrows, `u32` insertid, `u32` numfields, `u32`
    numrows, 12 reserved bytes.
  - if errno `!= 0`: one block (the query's own error message), nothing
    else.
  - else if numfields `> 0`: `numfields` × (name block, table block, `u32`
    type, `u32` flags, `u32` length), then `numrows × numfields` blocks
    in row-major order (a `NULL` block marks a SQL NULL value).
  - else (no result set, e.g. an `UPDATE`/`INSERT`): a single
    informational block (MySQL's own "N rows affected" string, or empty).

For mysql/pgsql, this extension always sends `SET NAMES '<charset>'` as an
extra query ahead of whatever the caller asked for (matching
`px.ntunnel.class.php::query()`/`multiquery()` exactly) and discards that
extra resultset from the returned array — the tunnel has no independent
"set the connection charset" action, this is the only way to control it.
**sqlite never gets this injected query** — there is no `SET NAMES`
equivalent for an embedded database (the tunnel is always UTF-8 there),
and `ntunnel_sqlite.php` has nothing that would even parse it as a no-op;
sending it would just waste a round-trip on a query that errors out.
