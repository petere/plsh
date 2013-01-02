PG_CONFIG = pg_config

pg_version := $(word 2,$(shell $(PG_CONFIG) --version))
extensions_supported = $(filter-out 6.% 7.% 8.% 9.0%,$(pg_version))
inline_supported = $(filter-out 6.% 7.% 8.%,$(pg_version))


MODULE_big = plsh
OBJS = plsh.o

extension_version = 2

DATA = $(if $(extensions_supported),plsh--unpackaged--1.sql plsh--1--2.sql,plsh.sql)
DATA_built = $(if $(extensions_supported),plsh--$(extension_version).sql)
EXTENSION = plsh

EXTRA_CLEAN = plsh.sql plsh--$(extension_version).sql

REGRESS = init function trigger crlf psql $(if $(inline_supported),inline)
REGRESS_OPTS = --inputdir=test


PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

override CFLAGS := $(filter-out -Wmissing-prototypes,$(CFLAGS))


all: plsh.sql

plsh.sql: $(if $(inline_supported),plsh-inline.sql,plsh-noinline.sql)
	cp $< $@

plsh--$(extension_version).sql: plsh.sql
	cp $< $@


version = $(shell git describe --tags)

dist:
	git archive --prefix=plsh-$(version)/ -o plsh-$(version).tar.gz -9 HEAD
