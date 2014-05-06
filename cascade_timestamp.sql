DROP FUNCTION IF EXISTS cascade_timestamp() CASCADE;

LOAD 'cascade_timestamp.so';

CREATE OR REPLACE FUNCTION cascade_timestamp()
RETURNS trigger AS 'cascade_timestamp.so'
LANGUAGE C;

