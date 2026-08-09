#!/bin/bash
set -eo pipefail

# NoteNest Database Migration Runner

DB_CONN=${DATABASE_URL:-"host=localhost port=5432 dbname=postgres user=postgres password=pass"}

# Parse DATABASE_URL key-value string into psql flags if necessary
if [[ "$DB_CONN" == host=* ]]; then
    eval export $(echo "$DB_CONN" | tr ' ' '\n')
    PSQL_CMD="psql -h $host -p $port -U $user -d $dbname"
    export PGPASSWORD=$password
else
    PSQL_CMD="psql $DB_CONN"
fi

# Ensure schema_migrations table exists
$PSQL_CMD -c "
CREATE TABLE IF NOT EXISTS schema_migrations (
    version VARCHAR(255) PRIMARY KEY,
    applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
" >/dev/null

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MIGRATIONS_DIR="$SCRIPT_DIR/migrations"

# Execute any unapplied SQL migration files in numerical order
for f in $(ls "$MIGRATIONS_DIR"/*.sql 2>/dev/null | sort); do
    filename=$(basename "$f")
    applied=$($PSQL_CMD -t -A -c "SELECT COUNT(*) FROM schema_migrations WHERE version = '$filename';" 2>/dev/null || echo "0")
    if [ "$applied" -eq 0 ]; then
        echo "Applying database migration: $filename"
        $PSQL_CMD -f "$f" >/dev/null
        $PSQL_CMD -c "INSERT INTO schema_migrations (version) VALUES ('$filename');" >/dev/null
    fi
done
