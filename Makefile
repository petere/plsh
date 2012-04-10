PG_CONFIG = pg_config

pg_version := $(word 2,$(shell $(PG_CONFIG) --version))
extensions_supported = $(filter-out 6.% 7.% 8.% 9.0%,$(pg_version))


MODULE_big = plsh
OBJS = plsh.o

extension_version = 1

DATA = $(if $(extensions_supported),plsh--unpackaged--1.sql plsh--$(extension_version).sql,plsh.sql)
DATA_built = $(if $(extensions_supported),plsh--$(extension_version).sql)
EXTENSION = plsh

EXTRA_CLEAN = plsh--$(extension_version).sql


PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

override CFLAGS := $(filter-out -Wmissing-prototypes,$(CFLAGS))


plsh--$(extension_version).sql: plsh.sql
	cp $< $@


version = $(shell git describe --tags)

dist:
	git archive --prefix=plsh-$(version)/ -o plsh-$(version).tar.gz -9 HEAD
