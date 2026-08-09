#!/bin/bash
set -e

psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" <<-EOSQL
DO \$\$
BEGIN
   IF NOT EXISTS (SELECT FROM pg_catalog.pg_roles WHERE rolname = 'replicator') THEN
      CREATE USER replicator WITH REPLICATION ENCRYPTED PASSWORD 'replpass';
   END IF;
END
\$\$;
EOSQL

echo "host replication all all trust" >> "$PGDATA/pg_hba.conf"
psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" -c "SELECT pg_reload_conf();"
