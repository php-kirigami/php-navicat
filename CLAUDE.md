# php-navicat

## Context

Part of the Kirigami ecosystem. Companion to `php-wasm-compiler`
(`../php-wasm-compiler`, github.com/php-kirigami/php-wasm-compiler), which
builds the `php.wasm` runtime and vendors third-party PHP extensions for
it (yaml, mdhtml, jsonk, apcu, sockets — see its own CLAUDE.md).

Navicat Premium ships a small PHP script under
`resource/httptunnel/ntunnel_mysql.php` (plus `ntunnel_pgsql.php` and
`ntunnel_sqlite.php` siblings) that a database administrator drops on a
web host to proxy a database connection over plain HTTP, for environments
where the real database port isn't directly reachable. The maintainer
already had a PHP-userland client for this protocol
(`px.ntunnel.class.php`, a `NTunnel`/`NTunnel_Resultset` class pair built
on `curl_*` + hand-rolled binary parsing via a handful of `ntunnel_read*`
helper functions and an unexplained `vopen()`/`vclose()` pair — almost
certainly a native "open a string as a readable stream handle" primitive
from some other extension, never actually needed here since this
extension reads the response buffer directly instead of wrapping it in a
stream).

This repo reimplements that client's network + binary-parsing core in C:
partly for raw performance, and specifically so it can be consumed by
`php-wasm-compiler` as a `mode: static` extension in `php.wasm` (that
repo's `WITH_CURL` block already links `libcurl.a` into the core build,
so this extension adds no new WASM-side dependency).

## Protocol reference (reverse-engineered directly from Navicat's own
server scripts — `ntunnel_mysql.php`/`ntunnel_pgsql.php`/`ntunnel_sqlite.php`,
read directly from a real `C:\Program Files\PremiumSoft\Navicat Premium
Lite 17\resource\httptunnel\` install, 2026-09-16)

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

## Decided architecture

1. **All three backends implemented (2026-09-16).** Originally scoped as
   "MySQL only for v1" (the note that used to be here assumed
   `ntunnel_pgsql.php`/`ntunnel_sqlite.php` would need only
   `navicat_build_fields()`-level changes, without having actually read
   either file yet) — revisited once the real scripts were read directly
   from a local Navicat Premium Lite 17 install. That assumption held for
   pgsql (see the Protocol reference section above: identical wire format,
   only the tunnel URL differs) but not for sqlite (a real per-value wire
   format difference, not just different POST fields). Added:
   - `navicat_pg_connect(string $url, string $host, int $port, string
     $user, string $password, string $db = '', array $options = [])` —
     same signature as `navicat_connect()`, sharing its entire
     implementation via a new `navicat_do_connect_hostbased()` static
     helper parameterized by backend (mysql/pgsql send identical POST
     fields, so no backend-specific branching was needed inside the
     connect logic itself, only in `navicat_build_fields()`).
   - `navicat_sqlite_connect(string $url, string $dbfile, array $options =
     [])` — a distinct signature (no host/port/user/password/db), since
     sqlite's connect semantics are genuinely different: `$options['create']
     = 'sqlite2'|'sqlite3'` selects `actn=2`/`actn=3` to create a brand
     new database file; omitting it uses `actn=C` against an existing file
     (server-side format auto-detection, per the Protocol reference
     section).
   - `navicat_connection` gained a `backend` enum field
     (`NAVICAT_BACKEND_MYSQL`/`PGSQL`/`SQLITE`) and a `dbfile` string
     (sqlite only). `navicat_build_fields()` branches on `backend` to
     decide which POST fields to send. `navicat_query()`/
     `navicat_multi_query()` branch on `backend != NAVICAT_BACKEND_SQLITE`
     to decide whether to inject `SET NAMES` and, correspondingly, which
     resultset index is the caller's real query (index `1`/all-but-`0`
     when injected, index `0`/all of them when not).
   - `navicat_read_resultset()` gained a `value_types` parameter (set from
     `conn->backend == NAVICAT_BACKEND_SQLITE` inside
     `navicat_run_queries()`): when true, an extra `u32` is read after
     every field value in every row (see the Protocol reference section)
     and surfaced as a parallel `"valuetypes"` array (row-major, alongside
     `"rows"`) in the resultset — sqlite's own field *header* type is a
     useless static placeholder (see above), so this is the only way a
     caller can actually learn a sqlite value's real per-value type;
     discarding those 4 bytes instead of surfacing them was considered and
     rejected as throwing away real information for no reason.
   - `phpinfo()`'s "navicat backends" row updated from `"mysql"` to
     `"mysql, pgsql, sqlite"`.
   - **Not yet tested against a real build or a real tunnel server** — see
     "Status" below. Written directly against the real
     `ntunnel_pgsql.php`/`ntunnel_sqlite.php` source (not guessed), and
     reviewed carefully by hand (brace/paren balance, every new
     `zval`/`zend_string` freed on every path) since Docker was occupied
     with another build in this session and a real compile+run pass — the
     same kind of check that already caught 3 real bugs in the original
     mysql implementation (see "Status" below) — could not be done yet.
2. **Minimal procedural API, not a class.** A connection is a plain Zend
   resource (`le_navicat_connection`) wrapping one reused `CURL*` easy
   handle plus the connection parameters (which must be resent on every
   request — see the protocol section above). No custom `zend_object`,
   no hand-rolled struct layout tricks — avoids the entire class of bugs
   `php-mdhtml`'s CLAUDE.md documents for `krakjoe/cmark`'s hand-mimicked
   `zend_object`. `escape()`/`insert()`/`update()`/`getrow()`/`getcol()`/
   `getvar()` from the original `NTunnel` class are left as PHP-userland
   convenience helpers building on `navicat_query()` (not reimplemented in
   C) — only the network + binary-parsing core moved.
3. **`mode: static` in `php-wasm-compiler`, fetched fresh from this repo's
   own tagged GitHub releases** — the same convention that repo already
   uses for `yaml`/`mdhtml`/`jsonk` (its `compile/php/Dockerfile` `wget`s a
   tagged source tarball, no local `COPY`). Chosen over `mode: shared`
   specifically because this extension needs `libcurl`, which is *already*
   statically linked into that build's core (`WITH_CURL`); building
   `navicat` static reuses that link directly instead of re-vendoring
   `libcurl` for a side module (the `vendorLib` mechanism `mode: shared`
   extensions like `sodium` use) — and sidesteps `php-wasm-compiler`'s own
   still-unconfirmed `mode: shared` runtime bug (missing
   `__stack_pointer`/`__table_base` exports on complex side modules, see
   its CLAUDE.md decisions 32/42) entirely, since a `mode: static`
   extension never goes through that code path.
4. **Also buildable `mode: shared` later, if ever needed.** Nothing about
   this extension's design (a standard Zend resource, no custom object
   layout) is `mode: static`-specific — the only reason it starts static
   is point 3's libcurl-reuse argument, not a technical constraint.
5. **Native build first, WASM second** — same order `php-mdhtml` used.
   `config.m4` mirrors `php-mdhtml`'s dual-path `PHP_ARG_WITH` shape: bare
   `--with-navicat` uses `pkg-config` to find the system `libcurl` (fast
   local iteration, no Docker/Emscripten round-trip); an explicit
   `--with-navicat=DIR` points at a pre-built `libcurl` prefix instead
   (`php-wasm-compiler`'s own `/root/lib` convention, where `libcurl.a` has
   no `pkg-config` file of its own).
6. **English for all repo content** (code, comments, docs, commit
   messages); conversation with the maintainer stays in French — same
   convention as every other repo in this ecosystem.
7. **License: GPL-2.0-or-later**, matching every other repo in the
   Kirigami ecosystem.

## Relationship to other repos

- **`php-wasm-compiler`** (github.com/php-kirigami/php-wasm-compiler,
  sibling checkout at `../php-wasm-compiler`): will consume this repo's
  tagged releases as a `mode: static` extension once this repo has a first
  working build and a real tag, the same way it already consumes
  `yaml`/`mdhtml`/`jsonk`.
- **Navicat Premium** (proprietary, PremiumSoft — not part of this repo):
  `resource/httptunnel/ntunnel_mysql.php` (and its pgsql/sqlite siblings)
  under its own install directory is the only protocol reference that
  exists; this repo's protocol section above is derived entirely from
  reading those scripts.

## Status

**✅ Native build succeeded and the wire protocol was validated end-to-end
(2026-09-16), against a real HTTP server -- not a simulation.** No real
Navicat/MySQL deployment was available, so a small mock tunnel server
(`resource/httptunnel/ntunnel_mysql.php`-alike, built directly from this
file's own protocol section above, scratch/not committed) was written and
served via PHP's built-in dev server (`php -S`); `navicat_connect()`/
`navicat_query()`/`navicat_multi_query()` were run against it for real,
over a real HTTP loopback connection, in a throwaway `php:8.4-cli` Docker
container (same technique `php-jsonk`'s own CLAUDE.md decision 17 used).
Exercised: `actn=C` connect + info parsing, a `SELECT`-shaped resultset
(2 fields, 2 rows, one row's value a real SQL `NULL`), an `UPDATE`-shaped
resultset (no fields, an info block), a server-side query error
(`errno != 0`), `multi_query()`'s correct discarding of the injected
`SET NAMES` resultset, `escape()` on both a string and a nested array, and
a deliberately truncated/malformed response (see the memory-safety fix
below).

**Three real bugs found and fixed by actually compiling and running this
code for the first time** -- proof, same as every other extension in this
ecosystem, that the "verified real API signatures" research alone wasn't
sufficient to trust without a real build:

1. **`zend_string_init(...)` missing its required third `persistent`
   argument at all 6 call sites** (`ZEND_STRL(...)` only expands to 2 of
   the 3 params the real PHP 8.x signature needs) -- a real compile error,
   not a style nit. Fixed by adding `, 0` (non-persistent) to every call.
2. **A real memory-safety bug: `navicat_read_resultsets()` could
   `zval_ptr_dtor()` an uninitialized zval.** `navicat_read_resultset()`
   used to call `array_init(out)` only *after* its first five `u32`
   reads succeeded; if the tunnel response was truncated before that
   point -- entirely network/server-controlled input, not something a
   caller can prevent -- the function returned 0 without ever
   initializing `out`, and its caller unconditionally dtor'd it anyway.
   Fixed by moving `array_init(out)` to the very first line of the
   function, before any read can fail, so the zval is always in a valid
   state regardless of where the function bails out. Verified directly:
   a mock response truncated mid-resultset-header (`FORCE_TRUNCATE`, 2
   bytes instead of the expected 32) now correctly makes
   `navicat_query()` return `false` with `navicat_last_error()` reporting
   "malformed resultset in tunnel response" -- no crash, process stayed
   alive.
3. **`navicat_escape()`'s own doc comment promised recursion into nested
   arrays; the code didn't actually do it.** A nested array's string
   values were `ZVAL_COPY`'d as-is (the same code path as non-string
   scalars), never actually escaped -- caught by a real test asserting on
   the *length* of an escaped nested string (`"c\"d"`, 3 chars in, 4
   chars -- `c`, `\`, `"`, `d` -- expected out; got 3 back, unescaped).
   Fixed by extracting the escaping logic into a new
   `navicat_escape_value()` helper that calls itself for array entries,
   and having `PHP_FUNCTION(navicat_escape)` just call it once at the top
   level (also removed a stray leftover `ZVAL_COPY(return_value, value)`
   line at the end of the old function body that would have silently
   clobbered the correctly-escaped result with the original, unescaped
   input).

**Not done yet:**

- `pgsql`/`sqlite` backends (point 1) — **✅ written (2026-09-16, see
  decision 1), but not yet compiled or run.** Docker was busy with another
  build in this session, so unlike the original mysql implementation
  (which found 3 real bugs by actually compiling/running it), this code
  has only been reviewed by hand so far. Treat it as unverified until a
  real build + a mock `ntunnel_pgsql.php`/`ntunnel_sqlite.php` test (same
  technique as the mysql one below) actually runs it.
- `config.w32` (Windows/PECL build parity) — not started; `php-mdhtml`
  added its own later, as a separate pass, once a Windows build was
  actually wanted.
- No GitHub tag/release yet — needed before `php-wasm-compiler` can fetch
  this repo the way it fetches `yaml`/`mdhtml`/`jsonk`.
- No WASM build attempted yet (point 3's whole reason for existing) — the
  native build above only proves the protocol/parsing logic; linking
  against `php-wasm-compiler`'s own `/root/lib` `libcurl.a` (the
  `--with-navicat=DIR` path in `config.m4`) is still untested.
- `vopen()`/`vclose()` from the original `px.ntunnel.class.php` were never
  implemented or needed here — this extension parses the curl response
  buffer directly rather than wrapping it in a stream handle.
- No real Navicat/MySQL server was available to test against — the mock
  tunnel server matches this file's own protocol write-up exactly, but
  that write-up was itself derived from reading `ntunnel_mysql.php` rather
  than from a byte capture of a real session, so a genuine end-to-end test
  against a real Navicat deployment is still worth doing before calling
  this production-ready.
- **Nothing in this repo is committed yet** — still the initial untracked
  working tree (`git status` shows zero commits). The fixes above are
  applied to the working files, not yet committed or tagged.
