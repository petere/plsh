MODULE_big = plsh
OBJS = plsh.o

DATA_built = createlang_pgplsh.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

version = $(shell git describe --tags)

dist:
	git archive --prefix=plsh-$(version)/ -o plsh-$(version).tar.gz -9 HEAD
