#!/bin/bash
set -e

# First-boot bootstrap for streaming replication. Schema migrations stay with
# db/migrate.sh (single runner path); this only provisions the replication
# role and its pg_hba entry.

psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" <<-EOSQL
DO \$\$
BEGIN
   IF NOT EXISTS (SELECT FROM pg_catalog.pg_roles WHERE rolname = 'replicator') THEN
      CREATE USER replicator WITH REPLICATION PASSWORD 'replpass';
   END IF;
END
\$\$;
EOSQL

# SCRAM only: no trust-based replication access.
grep -q "host replication replicator" "$PGDATA/pg_hba.conf" || \
    echo "host replication replicator all scram-sha-256" >> "$PGDATA/pg_hba.conf"

psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" -c "SELECT pg_reload_conf();" >/dev/null
