# Status

## Current state (2026-09-23, version 0.1.5)

- Statically linked into `@kirigami/php-wasm` by `php-wasm-compiler`
  (`navicat: { mode: static }`); the extension loads there (0.1.5), but
  no tunnel request has been made from the WASM build yet.
- Natively, all three backends (mysql, pgsql, sqlite) are verified
  end-to-end against real databases and Navicat's own unmodified
  `ntunnel_*.php` scripts (below).

## Build and verification history

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
