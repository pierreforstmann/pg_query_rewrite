# pg_query_rewrite Makefile

MODULES = pg_query_rewrite 

EXTENSION = pg_query_rewrite
DATA = pg_query_rewrite--0.0.1.sql
PGFILEDESC = "pg_query_rewrite - translate SQL statements"

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
