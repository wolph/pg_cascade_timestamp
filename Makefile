name		= cascade_timestamp

# SCRIPTS		= $(name)
MODULES		= $(name)
DATA		= $(name).sql
DOCS		= README.$(name)

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
