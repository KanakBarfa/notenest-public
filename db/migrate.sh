#!/bin/bash
set -euo pipefail

# NoteNest Database Migration Runner
# Each migration applies inside one transaction guarded by an advisory lock,
# so concurrent runners (app container, k8s jobs, ops shells) serialize and
# partially-applied migrations can never be recorded.

DB_CONN=${DATABASE_URL:-"host=localhost port=5432 dbname=postgres user=postgres password=pass"}

if [[ "$DB_CONN" == host=* ]]; then
    # libpq key=value string: pass straight through to psql.
    PSQL_CMD=(psql "$DB_CONN" -v ON_ERROR_STOP=1)
else
    PSQL_CMD=(psql "$DB_CONN" -v ON_ERROR_STOP=1)
fi

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MIGRATIONS_DIR="$SCRIPT_DIR/migrations"

apply_migration() {
    local f="$1"
    local filename
    filename=$(basename "$f")
    echo "Applying database migration: $filename"
    {
        echo "BEGIN;"
        echo "SELECT pg_advisory_xact_lock(hashtext('notenest-migrations'));"
        echo "INSERT INTO schema_migrations (version) VALUES ('$filename');"
        cat "$f"
        echo "COMMIT;"
    } | "${PSQL_CMD[@]}" -q >/dev/null
}

# Baseline table lives outside the per-migration transaction; creating it is
# idempotent, so racing CREATE TABLE IF NOT EXISTS statements are safe.
"${PSQL_CMD[@]}" -q -c "
CREATE TABLE IF NOT EXISTS schema_migrations (
    version VARCHAR(255) PRIMARY KEY,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
"

for f in $(ls "$MIGRATIONS_DIR"/*.sql 2>/dev/null | sort); do
    filename=$(basename "$f")
    applied=$("${PSQL_CMD[@]}" -t -A -c \
        "SELECT COUNT(*) FROM schema_migrations WHERE version = '$filename';")
    if [ "$applied" -eq 0 ]; then
        apply_migration "$f"
    fi
done

echo "Migrations up to date."
