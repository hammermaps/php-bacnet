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
  LMDB_SRC_DIR="$srcdir/deps/lmdb/libraries/liblmdb"

  if test ! -f "$BACNET_DIR/CMakeLists.txt"; then
    AC_MSG_ERROR([bacnet-stack source not found. Install from a PIE source package or initialize submodules.])
  fi
  if test ! -f "$LMDB_SRC_DIR/mdb.c"; then
    AC_MSG_ERROR([bundled LMDB source not found. Initialize submodules.])
  fi

  if test ! -f "$BACNET_BUILD_DIR/libbacnet-stack.a"; then
    AC_PATH_PROG([CMAKE], [cmake], [no])
    if test "$CMAKE" = "no"; then
      AC_MSG_ERROR([cmake is required to build the bundled bacnet-stack.])
    fi
    AC_MSG_NOTICE([building bundled bacnet-stack static library])
    mkdir -p "$BACNET_BUILD_DIR" || AC_MSG_ERROR([cannot create bacnet-stack build directory])
    "$CMAKE" -S "$BACNET_DIR" -B "$BACNET_BUILD_DIR" \
      -DBUILD_SHARED_LIBS=OFF \
      -DBACNET_STACK_BUILD_APPS=OFF \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF \
      -DBACNET_STACK_BUILD_TESTS=OFF \
      -DCMAKE_C_FLAGS=-fPIC || AC_MSG_ERROR([failed to configure bundled bacnet-stack])
    "$CMAKE" --build "$BACNET_BUILD_DIR" --parallel || AC_MSG_ERROR([failed to build bundled bacnet-stack])
  fi

  PHP_ADD_INCLUDE($BACNET_SRC_DIR)
  PHP_ADD_INCLUDE($LMDB_SRC_DIR)
  PHP_ADD_LIBRARY_WITH_PATH(bacnet-stack, $BACNET_BUILD_DIR, BACNET_SHARED_LIBADD)
  PHP_ADD_LIBRARY([pthread],, [BACNET_SHARED_LIBADD])
  PHP_SUBST(BACNET_SHARED_LIBADD)

  EXTRA_CFLAGS="-DBACDL_BIP -DBACNET_STACK_DEPRECATED_DISABLE"
  PHP_NEW_EXTENSION(bacnet,
    [bacnet.c
     src/bacnet_client.c
     src/bacnet_transport.c
     src/bacnet_classes.c
     src/bacnet_types.c
     src/bacnet_helpers.c
     src/bacnet_cache.c
     src/bacnet_security.c
     deps/lmdb/libraries/liblmdb/mdb.c
     deps/lmdb/libraries/liblmdb/midl.c],
    $ext_shared,
    ,
    $EXTRA_CFLAGS)
fi
