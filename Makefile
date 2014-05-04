MODULES = cascade_update_at
DOCS = README.cascade_update_at

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
