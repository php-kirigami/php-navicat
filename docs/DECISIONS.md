# Decisions

Numbered decision log, to be respected unless explicitly revisited.

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
