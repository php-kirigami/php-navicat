/*
  +----------------------------------------------------------------------+
  | php-navicat                                                           |
  +----------------------------------------------------------------------+
  | Copyright (c) Maxime Larrivée-Roy                                     |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2 of the GNU General Public   |
  | License, that is bundled with this package in the file LICENSE.      |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_NAVICAT_H
#define PHP_NAVICAT_H

extern zend_module_entry navicat_module_entry;
#define phpext_navicat_ptr &navicat_module_entry

#define PHP_NAVICAT_VERSION "0.1.3"

#ifdef PHP_WIN32
# define PHP_NAVICAT_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
# define PHP_NAVICAT_API __attribute__ ((visibility("default")))
#else
# define PHP_NAVICAT_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

PHP_MINIT_FUNCTION(navicat);
PHP_MSHUTDOWN_FUNCTION(navicat);
PHP_MINFO_FUNCTION(navicat);

#endif /* PHP_NAVICAT_H */
