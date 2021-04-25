# pg_query_rewrite Makefile

MODULES = pg_query_rewrite 

EXTENSION = pg_query_rewrite
DATA = pg_query_rewrite--0.0.1.sql
PGFILEDESC = "pg_query_rewrite - translate SQL statements"


REGRESS_OPTS = --temp-instance=/tmp/5454 --port=5454 --temp-config pg_query_rewrite.conf
REGRESS=test1 test2

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir=contrib/pg_query_rewrite
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

mycheck: 
	./mytest.sh
