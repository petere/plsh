AUTOMAKE_OPTIONS = foreign
EXTRA_DIST = test.sql createlang_pgplsh.sql.in

lib_LTLIBRARIES = pgplsh.la
pgplsh_la_SOURCES = pgplsh.c
pgplsh_la_LDFLAGS = -module -avoid-version

pkgdata_SCRIPTS = createlang_pgplsh.sql

createlang_pgplsh.sql: createlang_pgplsh.sql.in pgplsh.la
	.  pgplsh.la && sed "s,%filename%,$(libdir)/$$dlname,g" $(srcdir)/createlang_pgplsh.sql.in >createlang_pgplsh.sql

pkgdata_DATA = test.sql
