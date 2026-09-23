# Context

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

## Relationship to other repos

- **`php-wasm-compiler`** (github.com/php-kirigami/php-wasm-compiler,
  sibling checkout at `../php-wasm-compiler`): consumes this repo's tagged
  releases as a `mode: static` extension, the same way it consumes
  `yaml`/`mdhtml`/`jsonk`.
- **Navicat Premium** (proprietary, PremiumSoft — not part of this repo):
  `resource/httptunnel/ntunnel_mysql.php` (and its pgsql/sqlite siblings)
  under its own install directory is the only protocol reference that
  exists; this repo's protocol section above is derived entirely from
  reading those scripts.
