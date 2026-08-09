#!/bin/sh
set -e

# Checks PostgreSQL replication lag. Exits with 1 if lag exceeds 10MB (10485760 bytes).
MAX_LAG_BYTES=10485760
DB_HOST=${1:-db}
DB_USER=${DB_USER:-postgres}
DB_NAME=${DB_NAME:-postgres}

LAG=$(psql -h "$DB_HOST" -U "$DB_USER" -d "$DB_NAME" -t -c "SELECT COALESCE(MAX(pg_wal_lsn_diff(pg_current_wal_lsn(), replay_lsn)), 0) FROM pg_stat_replication;" 2>/dev/null | tr -d '[:space:]')

if [ -z "$LAG" ]; then
    # Standby node check
    IS_RECOVERY=$(psql -h "$DB_HOST" -U "$DB_USER" -d "$DB_NAME" -t -c "SELECT pg_is_in_recovery();" 2>/dev/null | tr -d '[:space:]')
    if [ "$IS_RECOVERY" = "t" ]; then
        exit 0
    fi
    exit 1
fi

LAG_INT=$(echo "$LAG" | cut -d'.' -f1)
if [ "$LAG_INT" -gt "$MAX_LAG_BYTES" ]; then
    echo "Replication lag ($LAG_INT bytes) exceeds threshold ($MAX_LAG_BYTES bytes)"
    exit 1
fi

exit 0
