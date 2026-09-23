# php-navicat

A PHP extension implementing a client for Navicat Premium's HTTP-tunnel
protocol (`ntunnel_mysql.php`/`ntunnel_pgsql.php`/`ntunnel_sqlite.php`) in
C, over libcurl. Part of the Kirigami ecosystem; shipped statically in
`@kirigami/php-wasm`.

This file is the entry point only. Detailed, evolving content lives under
[docs/](docs/), split by topic:

- [docs/CONTEXT.md](docs/CONTEXT.md) — origin (the userland `NTunnel`
  client) and relationship to other repos.
- [docs/PROTOCOL.md](docs/PROTOCOL.md) — the wire protocol, reverse-
  engineered from Navicat's own scripts.
- [docs/DECISIONS.md](docs/DECISIONS.md) — the numbered decision log.
- [docs/STATUS.md](docs/STATUS.md) — current state and verification
  history, including upstream bugs in Navicat's scripts.
- [docs/TODO.md](docs/TODO.md) — next actions.
- [docs/INSTRUCTIONS.md](docs/INSTRUCTIONS.md) — building and releasing.

## Core conventions

- **Language**: conversation with the user is in French; everything
  committed to this repo (code, comments, docs, commit messages) is in
  English.
- **API**: minimal procedural functions (`navicat_connect()`,
  `navicat_query()`, …), not a class (decision 3).
- **Upstream scripts**: Navicat's `ntunnel_*.php` are proprietary and not
  shipped here; work around their PHP 8 bugs on the client side only
  (decision 2), never by patching them.
- **Target**: PHP 8.5, native and Emscripten (`php-wasm-compiler`).
- **License**: GPL-2.0-or-later.
- Keep this file at or under ~200 lines. New durable content goes into
  the matching `docs/*.md` file.
