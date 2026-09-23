# Instructions

## Building natively

Needs PHP development headers and libcurl:

```bash
phpize
./configure --with-navicat
make
php -n -d extension=modules/navicat.so your-test.php
```

The end-to-end tests (see [STATUS.md](STATUS.md)) ran in Docker: real
`mysql:8` and `postgres:16-alpine` containers, plus a `php:8.4-cli`
container serving Navicat's `ntunnel_*.php` scripts with `php -S`. For a
PHP 8.5 toolchain without installing anything, see php-jsonk's
`docs/INSTRUCTIONS.md`.

## Releasing

1. Bump `PHP_NAVICAT_VERSION` in `php_navicat.h`.
2. Commit, then an annotated tag `vX.Y.Z`; push `main` with the tag.
3. `php-wasm-compiler` fetches tags by version (`matrix.json`); add the new
   tag there before the next PHP-WASM build.
