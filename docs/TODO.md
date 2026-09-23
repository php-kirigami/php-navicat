# TODO

- **Exercise the WASM build**: run `navicat_connect()`/`navicat_query()`
  from `@kirigami/php-wasm` against a real tunnel. It goes through the
  WASM libcurl, which now does HTTP and HTTPS end to end.
- **Real Navicat client test**: the server scripts and this extension were
  tested against real databases, but Navicat's own desktop client was
  never pointed at the same tunnel.
- **`config.w32`** (Windows/PECL build parity): not started.

Not needed: `vopen()`/`vclose()` from the original `px.ntunnel.class.php`;
this extension parses the curl response buffer directly.
