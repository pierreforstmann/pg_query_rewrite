# pgds Makefile

MODULES = pgds

EXTENSION = pgds
DATA = pgds--0.0.1.sql
PGFILEDESC = "pgds - DS"

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

#
pgxn:
	git archive --format zip  --output ../pgxn/pgds/pgds-0.0.1.zip main 
