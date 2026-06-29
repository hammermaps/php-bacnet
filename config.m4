PHP_ARG_WITH([bacnet], [for BACnet support],
  [AS_HELP_STRING([--with-bacnet], [Enable BACnet support])])

if test "$PHP_BACNET" != "no"; then
  AC_MSG_CHECKING([PHP version])
  PHP_BACNET_VERSION=`$PHP_CONFIG --version`
  if test -z "$PHP_BACNET_VERSION"; then
    AC_MSG_ERROR([php-config not found])
  fi
  PHP_BACNET_VERNUM=`$PHP_CONFIG --vernum`
  if test "$PHP_BACNET_VERNUM" -lt "80400"; then
    AC_MSG_ERROR([PHP 8.4+ required, found $PHP_BACNET_VERSION])
  fi
  AC_MSG_RESULT([$PHP_BACNET_VERSION])

  BACNET_DIR="$srcdir/deps/bacnet-stack"
  BACNET_BUILD_DIR="$srcdir/deps/bacnet-stack/build"
  BACNET_SRC_DIR="$BACNET_DIR/src"

  if test ! -f "$BACNET_BUILD_DIR/libbacnet-stack.a"; then
    AC_MSG_ERROR([libbacnet-stack.a not found. Run: ./scripts/build-deps.sh])
  fi

  PHP_ADD_INCLUDE($BACNET_SRC_DIR)
  PHP_ADD_LIBRARY_WITH_PATH(bacnet-stack, $BACNET_BUILD_DIR, BACNET_SHARED_LIBADD)
  PHP_SUBST(BACNET_SHARED_LIBADD)

  EXTRA_CFLAGS="-DBACDL_BIP -DBACNET_STACK_DEPRECATED_DISABLE"
  PHP_NEW_EXTENSION(bacnet,
    [bacnet.c
     src/bacnet_client.c
     src/bacnet_classes.c
     src/bacnet_types.c
     src/bacnet_helpers.c],
    $ext_shared,
    ,
    $EXTRA_CFLAGS)
fi
