dnl config.m4 for extension navicat

PHP_ARG_WITH([navicat],
  [for navicat support],
  [AS_HELP_STRING([--with-navicat[=DIR]],
    [Enable navicat support (a native client for Navicat's own HTTP-tunnel
     protocol; MySQL only for now). Reuses libcurl for the HTTP transport.
     Bare --with-navicat finds the system libcurl via pkg-config (native
     dev build); an explicit DIR points at a prefix containing include/
     and lib/ for a pre-built libcurl instead (e.g. php-wasm-compiler's
     own /root/lib convention, where libcurl is a static archive with no
     pkg-config file of its own)])],
  [no])

if test "$PHP_NAVICAT" != "no"; then
  AC_DEFINE(HAVE_NAVICAT, 1, [Whether you have navicat])

  if test "$PHP_NAVICAT" = "yes"; then
    dnl Native dev convenience: same discovery ext/curl's own config.m4 uses.
    PKG_CHECK_MODULES([NAVICAT_CURL], [libcurl])
    PHP_EVAL_INCLINE([$NAVICAT_CURL_CFLAGS])
    PHP_EVAL_LIBLINE([$NAVICAT_CURL_LIBS], [NAVICAT_SHARED_LIBADD])
  else
    dnl Explicit prefix (e.g. php-wasm-compiler's /root/lib): a pre-built
    dnl libcurl.a with no pkg-config file, so PKG_CHECK_MODULES can't see it.
    if test ! -f "$PHP_NAVICAT/lib/libcurl.a" && test ! -f "$PHP_NAVICAT/lib/libcurl.so"; then
      AC_MSG_ERROR([libcurl not found at $PHP_NAVICAT/lib -- pass bare --with-navicat to use pkg-config against the system libcurl, or a valid --with-navicat=DIR])
    fi
    PHP_ADD_INCLUDE([$PHP_NAVICAT/include])
    PHP_ADD_LIBRARY_WITH_PATH([curl], [$PHP_NAVICAT/lib], [NAVICAT_SHARED_LIBADD])
  fi

  PHP_SUBST(NAVICAT_SHARED_LIBADD)
  PHP_NEW_EXTENSION(navicat, navicat.c, $ext_shared)
fi
