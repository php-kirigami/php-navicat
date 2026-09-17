<div align="center">

<img src="https://zmotrin.github.io/assets/kirigami/kirigami-logo-universal.svg" alt="Kirigami" width="400" />

---

# php-navicat

A native PHP client for **Navicat's own HTTP-tunnel protocol** — MySQL, PostgreSQL, and SQLite.

[![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-yellow)](./LICENSE)
[![PHP 8.5](https://img.shields.io/badge/php-8.5-777bb4)](https://www.php.net/releases/8.5/)
[![Website](https://img.shields.io/badge/website-php--kirigami.github.io-1f6b4a)](https://php-kirigami.github.io)

</div>

---

## Overview

Navicat Premium ships a small PHP script (`ntunnel_mysql.php`, plus
`ntunnel_pgsql.php`/`ntunnel_sqlite.php` siblings) that database
administrators drop on a web host to proxy a database connection over
plain HTTP, for environments where the real database port isn't directly
reachable. `php-navicat` is a native client for that same protocol,
reverse-engineered directly from Navicat's own server scripts.

It exists to move the network + binary-parsing core of that client out of
PHP userland and into C — both for raw performance, and to make it
consumable from [`php-wasm-compiler`](https://github.com/php-kirigami/php-wasm-compiler)'s
`php.wasm` build as a `mode: static` extension, reusing the `libcurl`
already linked into that build's core.

Part of the **Kirigami** project ecosystem.

---

## Table of contents

- [php-navicat](#php-navicat)
  - [Overview](#overview)
  - [Table of contents](#table-of-contents)
  - [Status](#status)
  - [API](#api)
    - [`navicat_connect()`](#navicat_connect)
    - [`navicat_pg_connect()`](#navicat_pg_connect)
    - [`navicat_sqlite_connect()`](#navicat_sqlite_connect)
    - [`navicat_connection_info()`](#navicat_connection_info)
    - [`navicat_query()`](#navicat_query)
    - [`navicat_multi_query()`](#navicat_multi_query)
    - [`navicat_escape()`](#navicat_escape)
    - [`navicat_last_error()`](#navicat_last_error)
    - [`navicat_close()`](#navicat_close)
  - [Building from source](#building-from-source)
  - [Requirements](#requirements)
  - [Related](#related)
  - [License](#license)
  - [Author](#author)

---

## Status

Native build succeeds and the wire protocol has been validated end-to-end
against a mock tunnel server. See [CLAUDE.md](CLAUDE.md) for the full
protocol writeup and every decision made so far.

---

## API

Deliberately procedural, not a class — a connection is a plain PHP
resource wrapping one reused curl handle.

### `navicat_connect()`

```php
navicat_connect(
    string $url,
    string $host,
    int $port,
    string $user,
    string $password,
    string $db = '',
    array $options = []
): resource|false
```

Opens a connection and validates it against the tunnel (the protocol's
`actn=C` action). `$url` is the tunnel script's own URL (e.g.
`https://example.com/ntunnel_mysql.php`). `$options` accepts `charset`
(default `utf8`), `base64` (default `true`), `timeout` (default `600`),
`conntimeout` (default `30`), and `proxy`.

### `navicat_pg_connect()`

```php
navicat_pg_connect(
    string $url,
    string $host,
    int $port,
    string $user,
    string $password,
    string $db = '',
    array $options = []
): resource|false
```

Same signature, same options, and the same wire protocol as
`navicat_connect()` — Navicat's `ntunnel_pgsql.php` takes the exact same
POST fields as `ntunnel_mysql.php` (confirmed by reading both directly).
Only the URL you point `$url` at (your own `ntunnel_pgsql.php`) actually
selects the backend. `$db` defaults to Postgres' own `template1` on the
server side when left empty.

### `navicat_sqlite_connect()`

```php
navicat_sqlite_connect(
    string $url,
    string $dbfile,
    array $options = []
): resource|false
```

Connects to a SQLite database file (a server-side path, resolved by the
tunnel script — no host/port/credentials). By default (`$options` without
`create`) this tests/uses an *existing* file, auto-detected server-side as
SQLite2 or SQLite3 from its own header bytes. Pass `$options['create'] =
'sqlite2'` or `'sqlite3'` to instead create a brand-new database file (only
valid when `$dbfile` doesn't already exist). `$options` also accepts
`base64`, `timeout`, `conntimeout`, and `proxy` (no `charset` — sqlite has
no connection-charset concept, see [`navicat_query()`](#navicat_query)).

### `navicat_connection_info()`

```php
navicat_connection_info(resource $connection): array|false
```

Returns `['host' => ..., 'proto' => ..., 'version' => ...]` as reported by
the remote database, captured at `navicat_connect()` time.

### `navicat_query()`

```php
navicat_query(resource $connection, string $query): array|false
```

Runs one query and returns its resultset: `status`, `errno`, `errmsg`,
`affectrows`, `insertid`, `numfields`, `numrows`, and — when
`numfields > 0` — `fields` (per-column name/table/type/flags/length),
`fieldnames`, `rows` (each a `fieldname => value` associative array), and,
**for `navicat_sqlite_connect()` connections only**, `valuetypes` — a
row-major array (parallel to `rows`) of each value's real SQLite type
code. SQLite has per-value dynamic typing, so — unlike mysql/pgsql — its
`fields[]` header type is a useless static placeholder; `valuetypes` is
the only place a sqlite result's real per-value type is available.

For mysql/pgsql connections, this always runs an extra `SET NAMES
'<charset>'` ahead of your query and discards its resultset — the tunnel
has no independent "set the connection charset" action. sqlite connections
never send this (no such statement, no connection-charset concept there).

### `navicat_multi_query()`

```php
navicat_multi_query(resource $connection, array $queries): array|false
```

Runs several queries in one request; returns an array of resultsets (same
shape as above), one per query, in order.

### `navicat_escape()`

```php
navicat_escape(mixed $value): mixed
```

Escapes a string for safe interpolation into a query sent over the
tunnel (`\`, NUL, `\n`, `\r`, `'`, `"`, `\x1a`); applied recursively to
arrays. Non-string, non-array, and empty-string values pass through
unchanged.

### `navicat_last_error()`

```php
navicat_last_error(resource $connection): ?string
```

The connection's last transport- or protocol-level error message, or
`null` if the last operation succeeded.

### `navicat_close()`

```php
navicat_close(resource $connection): void
```

Closes the connection explicitly. Optional — a connection also closes
when its resource is garbage-collected.

---

## Building from source

Native build (fast iteration, no Docker/Emscripten):

```bash
phpize
./configure --with-navicat
make
```

`--with-navicat` (bare) finds the system `libcurl` via `pkg-config`. Pass
`--with-navicat=DIR` to point at a pre-built `libcurl` elsewhere instead
(e.g. `php-wasm-compiler`'s own `/root/lib` convention).

---

## Requirements

- PHP `>= 8.5`
- A C compiler, `phpize`
- `libcurl` (development headers, for the native build)

---

## Related

- [Navicat Premium](https://www.navicat.com/en/products/navicat-premium) —
  ships the `ntunnel_*.php` server scripts this extension talks to
- [php-wasm-compiler](https://github.com/php-kirigami/php-wasm-compiler) —
  builds the `php.wasm` runtime this extension will target as a
  `mode: static` extension

---

## License

GPL-2.0-or-later. See [LICENSE](./LICENSE) for the full text.

## Author

Maxime Larrivée-Roy
