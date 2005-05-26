AC_INIT(pgplsh, 1.0-7.3, [peter_e@gmx.net])
AM_INIT_AUTOMAKE
AC_CONFIG_SRCDIR(pgplsh.c)

AM_MAINTAINER_MODE

AC_PROG_CC
AC_DISABLE_STATIC
AM_PROG_LIBTOOL
AC_SUBST(LIBTOOL_DEPS)

AC_PATH_PROGS(PG_CONFIG, pg_config)
if test -z "$PG_CONFIG" ; then
  AC_MSG_ERROR([pg_config not found])
fi
postgresql_includes=`$PG_CONFIG --includedir`
AC_MSG_NOTICE([using PostgreSQL headers at $postgresql_includes])
postgresql_server_includes=`$PG_CONFIG --includedir-server`
AC_MSG_NOTICE([using PostgreSQL server headers at $postgresql_server_includes])

if test -n "$postgresql_includes"; then
  CPPFLAGS="-I$postgresql_includes $CPPFLAGS"
fi
if test -n "$postgresql_server_includes"; then
  CPPFLAGS="-I$postgresql_server_includes $CPPFLAGS"
fi

if test "$GCC" = yes; then
  CFLAGS="$CFLAGS -Wall -W"
fi

AC_CONFIG_FILES(Makefile)
AC_OUTPUT
