#!/bin/sh
set -e

# Real replication-lag check, run on a STANDBY.
# pg_stat_replication only has rows on the primary, so the standby instead
# compares its replayed LSN against the primary's current WAL LSN and fails
# when the byte distance exceeds the threshold.

MAX_LAG_BYTES=10485760
PRIMARY_HOST=${1:-db}
DB_USER=${DB_USER:-postgres}
DB_NAME=${DB_NAME:-postgres}
PGPASSWORD_LOCAL=${DB_PASSWORD:-pass}
export PGPASSWORD="$PGPASSWORD_LOCAL"

LOCAL_CONN="-h 127.0.0.1 -U $DB_USER -d $DB_NAME"

IS_RECOVERY=$(psql $LOCAL_CONN -t -A -c "SELECT pg_is_in_recovery();" 2>/dev/null | tr -d '[:space:]')
if [ "$IS_RECOVERY" != "t" ]; then
    echo "Node is not in recovery; lag check not applicable"
    exit 0
fi

REPLAY_LSN=$(psql $LOCAL_CONN -t -A -c "SELECT COALESCE(pg_last_wal_replay_lsn(), '0/0');" 2>/dev/null | tr -d '[:space:]')
if [ -z "$REPLAY_LSN" ]; then
    echo "Could not read local replay LSN"
    exit 1
fi

PRIMARY_LSN=$(psql -h "$PRIMARY_HOST" -U "$DB_USER" -d "$DB_NAME" -t -A \
    -c "SELECT pg_current_wal_lsn();" 2>/dev/null | tr -d '[:space:]')
if [ -z "$PRIMARY_LSN" ]; then
    echo "Could not read primary WAL LSN"
    exit 1
fi

LAG_BYTES=$(psql $LOCAL_CONN -t -A \
    -c "SELECT pg_wal_lsn_diff('$PRIMARY_LSN'::pg_lsn, '$REPLAY_LSN'::pg_lsn);" 2>/dev/null | cut -d'.' -f1)
if [ -z "$LAG_BYTES" ]; then
    echo "Could not compute replay distance"
    exit 1
fi

if [ "$LAG_BYTES" -gt "$MAX_LAG_BYTES" ]; then
    echo "Replication lag ($LAG_BYTES bytes) exceeds threshold ($MAX_LAG_BYTES bytes)"
    exit 1
fi

exit 0
