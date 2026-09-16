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
Navicat Premium Lite 17, `resource/httptunnel/`)

The wire format is **identical across all three backends** — only the
POST fields sent and the per-column type-code mapping differ. This repo
implements the MySQL backend only (see "Decided architecture" below).

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

This extension always sends `SET NAMES '<charset>'` as an extra query
ahead of whatever the caller asked for (matching
`px.ntunnel.class.php::query()`/`multiquery()` exactly) and discards that
extra resultset from the returned array — the tunnel has no independent
"set the connection charset" action, this is the only way to control it.

## Decided architecture

1. **MySQL only for v1.** `ntunnel_pgsql.php`/`ntunnel_sqlite.php` use the
   exact same wire format (see above) — only the POST fields sent differ
   (a libpq-style connstring for pgsql; `dbfile` + an `actn` of `2`/`3` to
   pick the SQLite driver generation for sqlite). Adding either later is
   almost entirely `navicat_build_fields()`-level work, not a new parser.
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

- `pgsql`/`sqlite` backends (point 1).
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
