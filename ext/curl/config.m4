dnl
dnl $Id$ 
dnl

PHP_ARG_WITH(curl, for cURL support,
[  --with-curl[=DIR]         Include cURL support])

if test "$PHP_CURL" != "no"; then
  if test -r $PHP_CURL/include/curl/easy.h; then
    CURL_DIR=$PHP_CURL
  else
    AC_MSG_CHECKING(for cURL in default path)
    for i in /usr/local /usr; do
      if test -r $i/include/curl/easy.h; then
        CURL_DIR=$i
        AC_MSG_RESULT(found in $i)
        break
      fi
    done
    if test -z "$CURL_DIR"; then
      AC_MSG_RESULT(not found)
      if which dpkg-architecture>/dev/null; then
        AC_MSG_CHECKING(for cURL in multiarch path)
        CURL_MULTIARCH_INCLUDE=/usr/include/$(dpkg-architecture -qDEB_HOST_MULTIARCH)
        if test -r $CURL_MULTIARCH_INCLUDE/curl/easy.h; then
          CURL_DIR=/usr
          AC_MSG_RESULT(found in $CURL_MULTIARCH_INCLUDE)
        else
          AC_MSG_RESULT(not found)
        fi
      fi
    fi
  fi

  if test -z "$CURL_DIR"; then
    AC_MSG_ERROR(Could not find cURL, please reinstall the libcurl distribution -dnl
 easy.h should be in <curl-dir>/include/curl/)
  fi

  CURL_CONFIG="curl-config"
  AC_MSG_CHECKING(for cURL 7.10.5 or greater)

  if ${CURL_DIR}/bin/curl-config --libs > /dev/null 2>&1; then
    CURL_CONFIG=${CURL_DIR}/bin/curl-config
  else
    if ${CURL_DIR}/curl-config --libs > /dev/null 2>&1; then
      CURL_CONFIG=${CURL_DIR}/curl-config
    fi
  fi

  curl_version_full=`$CURL_CONFIG --version`
  curl_version=`echo ${curl_version_full} | sed -e 's/libcurl //' | $AWK 'BEGIN { FS = "."; } { printf "%d", ($1 * 1000 + $2) * 1000 + $3;}'`
  if test "$curl_version" -ge 7010005; then
    AC_MSG_RESULT($curl_version_full)
    CURL_LIBS=`$CURL_CONFIG --libs`
  else
    AC_MSG_ERROR(cURL version 7.10.5 or later is required to compile php with cURL support)
  fi

  if test -z "$CURL_MULTIARCH_INCLUDE"; then
    PHP_ADD_INCLUDE($CURL_DIR/include)
  else
    PHP_ADD_INCLUDE($CURL_MULTIARCH_INCLUDE)
  fi
  PHP_EVAL_LIBLINE($CURL_LIBS, CURL_SHARED_LIBADD)
  PHP_ADD_LIBRARY_WITH_PATH(curl, $CURL_DIR/$PHP_LIBDIR, CURL_SHARED_LIBADD)
  
  AC_MSG_CHECKING([for SSL support in libcurl])
  CURL_SSL=`$CURL_CONFIG --feature | $EGREP SSL`
  if test "$CURL_SSL" = "SSL"; then
    AC_MSG_RESULT([yes])
    AC_DEFINE([HAVE_CURL_SSL], [1], [Have cURL with  SSL support])
   
    save_CFLAGS="$CFLAGS"
    CFLAGS="`$CURL_CONFIG --cflags`"
    save_LDFLAGS="$LDFLAGS"
    LDFLAGS="`$CURL_CONFIG --libs`"
   
    AC_PROG_CPP
    AC_MSG_CHECKING([for openssl support in libcurl])
    AC_TRY_RUN([
#include <curl/curl.h>

int main(int argc, char *argv[])
{
  curl_version_info_data *data = curl_version_info(CURLVERSION_NOW);

  if (data && data->ssl_version && *data->ssl_version) {
    const char *ptr = data->ssl_version;

    while(*ptr == ' ') ++ptr;
    return strncasecmp(ptr, "OpenSSL", sizeof("OpenSSL")-1);
  }
  return 1;
}
    ],[
      AC_MSG_RESULT([yes])
      AC_CHECK_HEADERS([openssl/crypto.h], [
        AC_DEFINE([HAVE_CURL_OPENSSL], [1], [Have cURL with OpenSSL support])
      ])
    ], [
      AC_MSG_RESULT([no])
    ], [
      AC_MSG_RESULT([no])
    ])
   
    AC_MSG_CHECKING([for gnutls support in libcurl])
    AC_TRY_RUN([
#include <curl/curl.h>

int main(int argc, char *argv[])
{
  curl_version_info_data *data = curl_version_info(CURLVERSION_NOW);
  
  if (data && data->ssl_version && *data->ssl_version) {
    const char *ptr = data->ssl_version;

    while(*ptr == ' ') ++ptr;
    return strncasecmp(ptr, "GnuTLS", sizeof("GnuTLS")-1);
  }
  return 1;
}
], [
      AC_MSG_RESULT([yes])
      AC_CHECK_HEADER([gcrypt.h], [
        AC_DEFINE([HAVE_CURL_GNUTLS], [1], [Have cURL with GnuTLS support])
      ])
    ], [
      AC_MSG_RESULT([no])
    ], [
      AC_MSG_RESULT([no])
    ])
   
    CFLAGS="$save_CFLAGS"
    LDFLAGS="$save_LDFLAGS"
  else
    AC_MSG_RESULT([no])
  fi

  PHP_CHECK_LIBRARY(curl,curl_easy_perform, 
  [ 
    AC_DEFINE(HAVE_CURL,1,[ ])
  ],[
    AC_MSG_ERROR(There is something wrong. Please check config.log for more information.)
  ],[
    $CURL_LIBS -L$CURL_DIR/$PHP_LIBDIR
  ])

  PHP_CHECK_LIBRARY(curl,curl_easy_strerror,
  [
    AC_DEFINE(HAVE_CURL_EASY_STRERROR,1,[ ])
  ],[],[
    $CURL_LIBS -L$CURL_DIR/$PHP_LIBDIR
  ])

  PHP_CHECK_LIBRARY(curl,curl_multi_strerror,
  [
    AC_DEFINE(HAVE_CURL_MULTI_STRERROR,1,[ ])
  ],[],[
    $CURL_LIBS -L$CURL_DIR/$PHP_LIBDIR
  ])

  PHP_NEW_EXTENSION(curl, interface.c multi.c share.c curl_file.c, $ext_shared)
  PHP_SUBST(CURL_SHARED_LIBADD)
fi
