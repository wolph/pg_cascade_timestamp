DROP FUNCTION IF EXISTS cascade_update_at() CASCADE;

LOAD 'cascade_update_at.so';

CREATE OR REPLACE FUNCTION cascade_update_at()
RETURNS trigger AS 'cascade_update_at.so'
LANGUAGE C;

