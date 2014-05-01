MODULES = cascade_update_at
# DATA_built = cascade_update_at.sql
DOCS = README.cascade_update_at

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
