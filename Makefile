# pg_query_rewrite Makefile

MODULES = pg_query_rewrite 

EXTENSION = pg_query_rewrite
DATA = pg_query_rewrite--0.0.1.sql
PGFILEDESC = "pg_query_rewrite - translate SQL statements"


REGRESS_OPTS = --temp-instance=/tmp/5454 --port=5454 --temp-config pg_query_rewrite.conf
REGRESS=test0 test1 test2 test3 test4 test5 test6 test7

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

#
pgxn:
	git archive --format zip  --output ../pgxn/pg_query_rewrite/pg_query_rewrite-0.0.4.zip master
