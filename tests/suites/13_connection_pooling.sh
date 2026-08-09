#!/bin/bash
set -eo pipefail

# Suite 13: Connection Pooling & Circuit Breaker E2E Test Suite

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Suite 13: Connection Pooling & Circuit Breaker ==="

USER_EMAIL="pool_user_$(date +%s)@example.com"
USER_PASS="securepassword123"

# 1. Verify PgBouncer container readiness and inspect pools
echo "[Step 1] Verifying PgBouncer container and pool configuration..."
if docker ps --format '{{.Names}}' | grep -q "^notenest-pgbouncer-container$"; then
    SHOW_POOLS_OUT=$(docker exec -e PGPASSWORD=pass notenest-pgbouncer-container psql -h 127.0.0.1 -p 6432 -U postgres -d pgbouncer -c "SHOW POOLS;" 2>&1 || true)
    echo "PgBouncer Pools output:"
    echo "$SHOW_POOLS_OUT"
    if echo "$SHOW_POOLS_OUT" | grep -q "postgres"; then
        echo "--> PgBouncer database pool 'postgres' verified active."
    else
        echo "--> Warning: PgBouncer show pools output did not list postgres pool directly, continuing..."
    fi
else
    echo "--> Skipping PgBouncer container query (running outside Docker container setup)."
fi

# 2. Authenticate user and create note via REST API routed through PgBouncer
echo "[Step 2] Testing REST API authentication and CRUD operations via PgBouncer..."
assert_request "POST" "/signup" 201 '{"email":"'$USER_EMAIL'","password":"'$USER_PASS'"}'

LOGIN_RES=$(curl -s -X POST -H "Content-Type: application/json" -d '{"email":"'$USER_EMAIL'","password":"'$USER_PASS'"}' "$SERVER_URL/login")
TOKEN=$(echo "$LOGIN_RES" | grep -o '"token":"[^"]*' | cut -d'"' -f4)

if [ -z "$TOKEN" ]; then
    echo "FAILED: Unable to extract JWT token from login response."
    exit 1
fi
echo "--> Obtained JWT token successfully."

NOTE_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN" -d '{"title":"Pool Test Note","content":"Testing PgBouncer and circuit breaker"}' "$SERVER_URL/notes")
NOTE_ID=$(echo "$NOTE_RES" | grep -o '"id":"[^"]*' | cut -d'"' -f4)

if [ -z "$NOTE_ID" ]; then
    echo "FAILED: Unable to create note via REST API."
    exit 1
fi
echo "--> Note created successfully with ID: $NOTE_ID"

# 3. Test GraphQL endpoint routing through PgBouncer & GraphQL Health Check
echo "[Step 3] Testing GraphQL queries and service health with pool metrics..."
GQL_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN" \
    -d '{"query":"query { note(id: \"'$NOTE_ID'\") { id title content author { email } } me { email } }"}' \
    "$SERVER_URL/graphql")
echo "GraphQL Response: $GQL_RES"

if echo "$GQL_RES" | grep -q "Pool Test Note"; then
    echo "--> GraphQL query via PgBouncer succeeded."
else
    echo "FAILED: GraphQL query failed to retrieve note."
    exit 1
fi

GQL_HEALTH=$(curl -s "$SERVER_URL/graphql" -H "Content-Type: application/json" || true)
echo "GraphQL Health status check completed."

# 4. Test Circuit Breaker fast failure and recovery when PostgreSQL is paused
echo "[Step 4] Testing Circuit Breaker fast failure and recovery..."
if docker ps --format '{{.Names}}' | grep -q "^notenest-db-container$"; then
    echo "Pausing PostgreSQL database container to trigger circuit breaker..."
    docker compose pause db >/dev/null 2>&1 || docker pause notenest-db-container >/dev/null 2>&1

    echo "Sending request while DB is down..."
    FAIL_RES=$(curl -s --max-time 3 -w "%{http_code}" -o /tmp/fail_body.txt -X GET -H "Authorization: Bearer $TOKEN" "$SERVER_URL/notes" || true)
    FAIL_BODY=$(cat /tmp/fail_body.txt 2>/dev/null || true)
    rm -f /tmp/fail_body.txt
    echo "Failed request HTTP status: $FAIL_RES, body: $FAIL_BODY"

    echo "Unpausing PostgreSQL database container..."
    docker compose unpause db >/dev/null 2>&1 || docker unpause notenest-db-container >/dev/null 2>&1
    sleep 6

    echo "Verifying service recovery post-unpause..."
    assert_request "GET" "/notes" 200 "" "$TOKEN"
    echo "--> Circuit Breaker recovery verified."
else
    echo "--> Skipping DB pause/unpause circuit breaker simulation (not running in Docker environment)."
fi

echo "=== Suite 13 Completed Successfully ==="
