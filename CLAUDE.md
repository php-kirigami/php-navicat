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
   - **✅ Fully verified end-to-end once Docker freed up (2026-09-16) — see
     "Status" below for the full writeup.** Two more real bugs were found
     this way (both in Navicat's own official tunnel scripts, not in this
     extension — see decision 2 below for the fix and full details):
     `encodeBase64=1` with no queries crashes `ntunnel_mysql.php`/
     `ntunnel_pgsql.php`/`ntunnel_sqlite.php` outright on PHP 8+
     (`count(null)`), and a genuine SQL error crashes `ntunnel_pgsql.php`
     specifically (`pg_affected_rows(false)`, a function whose parameter
     became strictly typed in PHP 8.1). Both were caught by running this
     code for real, exactly the outcome that motivated flagging this as
     "unverified" in the first place — proof, again, that reviewing
     carefully by hand is not a substitute for actually compiling and
     running.
2. **`encodeBase64` is always sent as `"0"` for connect-time requests
   (`actn=C`/`2`/`3`, i.e. whenever there are no queries), regardless of
   the connection's own base64 setting — a real bug fix, not a style
   choice.** Found while running the pgsql/sqlite end-to-end test below:
   `navicat_connect()`'s own default (`use_base64 = 1`, inherited from
   `px.ntunnel.class.php`'s own default) makes every connect request send
   `encodeBase64=1` — and Navicat's own, current, unmodified
   `ntunnel_*.php` scripts all do
   `if ($_POST["encodeBase64"] == '1') { for ($i = 0; $i < count($_POST["q"]); ...) }`
   unconditionally, with no `isset()`/`is_array()` guard on `$_POST["q"]`
   first. A connect request never sends any `q[]` fields at all, so
   `$_POST["q"]` is `null`, and `count(null)` — a silent `0`-returning
   warning on PHP 7 — is a **fatal, uncaught `TypeError` as of PHP 8.0**.
   Confirmed directly, twice: a raw `curl` POST with `encodeBase64=0`
   against a real `ntunnel_mysql.php` on PHP 8.4 returns a clean, correct
   51-byte binary response; the identical request with `encodeBase64=1`
   instead returns nothing (the script dies) and the PHP dev server's own
   log shows the exact `TypeError` at the `count($_POST["q"])` line. Since
   there is nothing to decode on a connect request regardless of this
   setting, forcing `"0"` there changes no real behavior — real query
   requests (`actn=Q`, `queries_count > 0`) still honor
   `conn->use_base64` exactly as before. This means any Navicat tunnel
   host running PHP 8+ was previously **unreachable at all** through this
   extension's default options, not just "using base64 needlessly" — a
   correctness bug, not a minor inefficiency.
3. **Minimal procedural API, not a class.** A connection is a plain Zend
   resource (`le_navicat_connection`) wrapping one reused `CURL*` easy
   handle plus the connection parameters (which must be resent on every
   request — see the protocol section above). No custom `zend_object`,
   no hand-rolled struct layout tricks — avoids the entire class of bugs
   `php-mdhtml`'s CLAUDE.md documents for `krakjoe/cmark`'s hand-mimicked
   `zend_object`. `escape()`/`insert()`/`update()`/`getrow()`/`getcol()`/
   `getvar()` from the original `NTunnel` class are left as PHP-userland
   convenience helpers building on `navicat_query()` (not reimplemented in
   C) — only the network + binary-parsing core moved.
4. **`mode: static` in `php-wasm-compiler`, fetched fresh from this repo's
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
5. **Also buildable `mode: shared` later, if ever needed.** Nothing about
   this extension's design (a standard Zend resource, no custom object
   layout) is `mode: static`-specific — the only reason it starts static
   is point 4's libcurl-reuse argument, not a technical constraint.
6. **Native build first, WASM second** — same order `php-mdhtml` used.
   `config.m4` mirrors `php-mdhtml`'s dual-path `PHP_ARG_WITH` shape: bare
   `--with-navicat` uses `pkg-config` to find the system `libcurl` (fast
   local iteration, no Docker/Emscripten round-trip); an explicit
   `--with-navicat=DIR` points at a pre-built `libcurl` prefix instead
   (`php-wasm-compiler`'s own `/root/lib` convention, where `libcurl.a` has
   no `pkg-config` file of its own).
7. **English for all repo content** (code, comments, docs, commit
   messages); conversation with the maintainer stays in French — same
   convention as every other repo in this ecosystem.
8. **License: GPL-2.0-or-later**, matching every other repo in the
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

**✅✅ `pgsql`/`sqlite` backends fully verified end-to-end (2026-09-16),
against real MySQL/PostgreSQL servers and the real, unmodified, proprietary
`ntunnel_*.php` scripts copied directly from a real Navicat Premium Lite
17 install (`C:\Program Files\PremiumSoft\Navicat Premium Lite
17\resource\httptunnel\`) — not mocks, and not the earlier
protocol-write-up-derived approximation the mysql backend was first
validated against.** Docker had been busy with another build when this
backend was first written (hence the "reviewed by hand, not run yet"
caveat that used to be here); once it freed up, the full setup was: a
`docker network` with real `mysql:8` and `postgres:16-alpine` containers,
a third `php:8.4-cli` container (with `mysqli`/`pgsql` built via
`docker-php-ext-install`) serving the three real `ntunnel_*.php` files
via `php -S`, running `navicat.so` against all three over real HTTP
loopback + real TCP to the two database containers, and a real SQLite
file for the sqlite backend (created fresh via `actn=3`, then reopened via
plain `actn=C` to prove the server's own file-format auto-detection
round-trips correctly). **37 real assertions, 0 failures** after two real
bugs were found and fixed (see decision 2's `encodeBase64` fix above) and
two more real, *upstream* (Navicat's own scripts, not this extension)
PHP-8-incompatibility bugs were found and precisely diagnosed rather than
worked around:

- `ntunnel_mysql.php`/`ntunnel_pgsql.php`/`ntunnel_sqlite.php` all crash
  outright (`count(): Argument #1 ($value) must be of type Countable|array,
  null given`) on any request with `encodeBase64=1` and no `q[]` fields —
  i.e. every connect request, on any host running PHP 8.0+. This is why
  decision 2's fix exists; confirmed via a raw `curl` request with the
  server's own log showing the exact uncaught `TypeError` and its file/line.
- `ntunnel_pgsql.php` additionally crashes (`pg_affected_rows(): Argument
  #1 ($result) must be of type PgSql\Result, false given`) on any genuine
  SQL error, on any host running PHP 8.1+ (where pgsql's functions gained
  strict object parameter types) — `pg_query()` returns `false` on error,
  and the script calls `pg_affected_rows()`/`pg_num_fields()`/
  `pg_num_rows()` on it unconditionally, with no `false` check first.
  `navicat_query()` correctly reports this as `false` (a truncated/
  malformed response, which is genuinely what it is once the script dies
  mid-response) rather than papering over it — there is nothing this
  extension could do differently here; the fix, if any, belongs in
  Navicat's own script, which this repo doesn't own or ship. Neither of
  these two script bugs is specific to the containers/versions used for
  this test — they're structural, triggered by any PHP 8+ (first one) or
  PHP 8.1+ (second one) host, which by 2026 is realistically most of them.
- A real, benign upstream quirk (not a bug, just worth knowing): against
  PostgreSQL 10+ (two-part version strings like `"16.4"`),
  `ntunnel_pgsql.php`'s own `sscanf($version, "%d.%d.%d", ...)`
  version-parsing (written for PostgreSQL 9.x's three-part `"9.6.3"`
  style) fails to extract 3 fields, and its own two-part fallback right
  after it *also* fails (no third `%s` component to match once the string
  is exhausted) — so the connect response's version block is the literal
  string `"0"` against any modern Postgres. `navicat_connection_info()`
  faithfully reports whatever the server actually sent; there's nothing to
  fix here on this extension's side.
- `mysql`'s own original mock-server-based validation (below) was also
  re-run against this session's real MySQL container as a regression
  check, since `navicat_build_fields()`/`navicat_read_resultset()`/
  `navicat_query()`/`navicat_multi_query()` were all touched by this same
  change — all still pass.

**Not done yet:**

- `config.w32` (Windows/PECL build parity) — not started; `php-mdhtml`
  added its own later, as a separate pass, once a Windows build was
  actually wanted.
- No GitHub tag/release yet — needed before `php-wasm-compiler` can fetch
  this repo the way it fetches `yaml`/`mdhtml`/`jsonk`.
- No WASM build attempted yet (point 4's whole reason for existing) — the
  native build above only proves the protocol/parsing logic; linking
  against `php-wasm-compiler`'s own `/root/lib` `libcurl.a` (the
  `--with-navicat=DIR` path in `config.m4`) is still untested.
- `vopen()`/`vclose()` from the original `px.ntunnel.class.php` were never
  implemented or needed here — this extension parses the curl response
  buffer directly rather than wrapping it in a stream handle.
- No real Navicat *client* was ever pointed at these tunnel scripts (this
  session validated the *server* scripts + this extension's own client
  against real databases, both real, but never Navicat's own official
  desktop client talking to the same tunnel) — a genuine end-to-end test
  against a real Navicat deployment on the *client* side is still worth
  doing before calling this production-ready, though the wire format is
  now about as independently verified as it can be without that.
