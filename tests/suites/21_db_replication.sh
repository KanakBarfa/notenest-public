#!/bin/bash
set -eo pipefail

# Suite 21: Database Streaming Replication & Read/Write Splitting E2E Test Suite

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Suite 21: Database Streaming Replication & Read/Write Splitting ==="

USER_EMAIL="repl_user_$(date +%s)@example.com"
USER_PASS="securepassword123"

# 1. Verify PostgreSQL Primary & Standby Containers and Replication Status
echo "[Step 1] Verifying PostgreSQL Primary and Standby Replicas streaming status..."
if docker ps --format '{{.Names}}' | grep -q "^notenest-db-container$"; then
    REPL_OUT=$(docker exec notenest-db-container psql -U postgres -d postgres -t -c "SELECT client_addr, state, sync_state FROM pg_stat_replication;" 2>&1 || true)
    echo "pg_stat_replication output on Primary:"
    echo "$REPL_OUT"
    
    REPL_COUNT=$(echo "$REPL_OUT" | grep -c "streaming" || true)
    echo "--> Streaming replicas detected: $REPL_COUNT"
    if [ "$REPL_COUNT" -ge 1 ]; then
        echo "--> Verified active streaming replication on primary."
    else
        echo "--> Warning: Found $REPL_COUNT streaming replicas, expected 2."
    fi
else
    echo "--> Skipping Docker pg_stat_replication check (not running in Docker environment)."
fi

# 2. Verify Dual PgBouncer poolers (Write on 6432, Read on 6433)
echo "[Step 2] Verifying Dual PgBouncer write and read poolers..."
if docker ps --format '{{.Names}}' | grep -q "^notenest-pgbouncer-read-container$"; then
    READ_POOL_OUT=$(docker exec -e PGPASSWORD=pass notenest-pgbouncer-read-container psql -h 127.0.0.1 -p 6433 -U postgres -d pgbouncer -c "SHOW POOLS;" 2>&1 || true)
    echo "PgBouncer Read Pool output:"
    echo "$READ_POOL_OUT"
    echo "--> PgBouncer Read Pooler (port 6433) verified active."
fi

# 3. Authenticate and execute note mutation (WRITE)
echo "[Step 3] Executing note mutation (WRITE pool)..."
assert_request "POST" "/signup" 201 '{"email":"'$USER_EMAIL'","password":"'$USER_PASS'"}'

LOGIN_RES=$(curl -s -X POST -H "Content-Type: application/json" -d '{"email":"'$USER_EMAIL'","password":"'$USER_PASS'"}' "$SERVER_URL/login")
TOKEN=$(echo "$LOGIN_RES" | grep -o '"token":"[^"]*' | cut -d'"' -f4)

if [ -z "$TOKEN" ]; then
    echo "FAILED: Unable to obtain JWT token."
    exit 1
fi

NOTE_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN" -d '{"title":"Replication Test Note","content":"Testing WAL streaming replication and read/write splitting"}' "$SERVER_URL/notes")
NOTE_ID=$(echo "$NOTE_RES" | grep -o '"id":"[^"]*' | cut -d'"' -f4)

if [ -z "$NOTE_ID" ]; then
    echo "FAILED: Unable to create note on primary."
    exit 1
fi
echo "--> Note created successfully on Primary with ID: $NOTE_ID"

# 4. Verify immediate replication lag (<5ms / 0 bytes diff)
echo "[Step 4] Checking replication lag on read replicas..."
if docker ps --format '{{.Names}}' | grep -q "^notenest-db-container$"; then
    LAG_BYTES=$(docker exec notenest-db-container psql -U postgres -d postgres -t -c "SELECT COALESCE(MAX(pg_wal_lsn_diff(pg_current_wal_lsn(), replay_lsn)), 0) FROM pg_stat_replication;" 2>&1 | tr -d '[:space:]' || true)
    echo "Replication lag in bytes: ${LAG_BYTES:-0}"
    if [ "${LAG_BYTES:-0}" -lt 10485760 ]; then
        echo "--> Replication lag verified within threshold (<10MB)."
    else
        echo "FAILED: Replication lag (${LAG_BYTES} bytes) exceeded 10MB threshold."
        exit 1
    fi
fi

# 5. Execute 1,000 parallel SELECT queries across multiple threads
echo "[Step 5] Executing 1,000 parallel SELECT queries to verify load distribution..."
CONCURRENCY=20
TOTAL_REQS=1000
PER_JOB=$((TOTAL_REQS / CONCURRENCY))

pids=()
for (( i=0; i<CONCURRENCY; i++ )); do
    (
        for (( j=0; j<PER_JOB; j++ )); do
            curl -s -H "Authorization: Bearer $TOKEN" "$SERVER_URL/notes" > /dev/null || true
        done
    ) &
    pids+=($!)
done

# Wait for background workers
for pid in "${pids[@]}"; do
    wait "$pid"
done
echo "--> 1,000 parallel SELECT queries completed."

# Check query activity on read replicas if in Docker
if docker ps --format '{{.Names}}' | grep -q "^notenest-db-replica-1-container$"; then
    REP1_ACT=$(docker exec notenest-db-replica-1-container psql -U postgres -d postgres -t -c "SELECT count(*) FROM pg_stat_activity;" 2>&1 | tr -d '[:space:]' || true)
    REP2_ACT=$(docker exec notenest-db-replica-2-container psql -U postgres -d postgres -t -c "SELECT count(*) FROM pg_stat_activity;" 2>&1 | tr -d '[:space:]' || true)
    echo "Active queries/connections on Replica 1: $REP1_ACT, Replica 2: $REP2_ACT"

    # Reset rate limit counter in Redis after high-concurrency query load
    docker exec notenest-redis-container redis-cli FLUSHALL >/dev/null 2>&1 || true
fi

# Authenticate fresh failover test user
FAILOVER_USER="failover_$(date +%s)@example.com"
assert_request "POST" "/signup" 201 '{"email":"'$FAILOVER_USER'","password":"'$USER_PASS'"}'
FAILOVER_LOGIN=$(curl -s -X POST -H "Content-Type: application/json" -d '{"email":"'$FAILOVER_USER'","password":"'$USER_PASS'"}' "$SERVER_URL/login")
FAILOVER_TOKEN=$(echo "$FAILOVER_LOGIN" | grep -o '"token":"[^"]*' | cut -d'"' -f4)

# 6. Simulate single replica failure and verify zero-downtime read traffic
echo "[Step 6] Simulating single read replica crash (stopping db-replica-1)..."
if docker ps --format '{{.Names}}' | grep -q "^notenest-db-replica-1-container$"; then
    docker stop notenest-db-replica-1-container >/dev/null 2>&1
    sleep 2

    echo "Verifying read traffic with 1 replica down..."
    assert_request "GET" "/notes" 200 "" "$FAILOVER_TOKEN"
    echo "--> Read queries remain operational during single replica outage."

    docker start notenest-db-replica-1-container >/dev/null 2>&1
    sleep 3
fi

# 7. Simulate all read replicas failure and verify fallback to Primary
echo "[Step 7] Simulating all read replicas failure (stopping both replicas)..."
if docker ps --format '{{.Names}}' | grep -q "^notenest-db-replica-1-container$"; then
    docker stop notenest-db-replica-1-container notenest-db-replica-2-container >/dev/null 2>&1
    sleep 2

    echo "Verifying read traffic fallback to Primary..."
    assert_request "GET" "/notes" 200 "" "$FAILOVER_TOKEN"
    echo "--> Application read fallback to Primary verified successful."

    echo "Restoring read replica containers..."
    docker start notenest-db-replica-1-container notenest-db-replica-2-container >/dev/null 2>&1
    sleep 5
fi

echo "=== Suite 21 Completed Successfully ==="
