#!/bin/bash
set -eo pipefail

# Common configuration and helper functions for NoteNest E2E tests

SERVER_URL=${SERVER_URL:-"http://localhost/api"}

# Helper to clean up database tables (only test-generated users and their cascading data)
cleanup_db() {
    local sql="TRUNCATE TABLE notes, users, outbox RESTART IDENTITY CASCADE;"
    if docker ps --format '{{.Names}}' 2>/dev/null | grep -qE "^(notenest-db-container|notenest-dev-postgres)$"; then
        local db_container
        db_container=$(docker ps --format '{{.Names}}' | grep -E "^(notenest-db-container|notenest-dev-postgres)$" | head -n1)
        local redis_container
        redis_container=$(docker ps --format '{{.Names}}' | grep -E "^(notenest-redis-container|notenest-dev-redis)$" | head -n1)
        echo "Cleaning up test-generated database records and redis cache..."
        docker exec -i "$db_container" psql -U postgres -d postgres -c "$sql" >/dev/null 2>&1 || true
        if [ -n "$redis_container" ]; then
            docker exec -i "$redis_container" redis-cli FLUSHALL >/dev/null 2>&1 || true
        fi
    else
        echo "Cleaning up local test database records..."
        DB_CONN=${DATABASE_URL:-"host=localhost port=5432 dbname=postgres user=postgres password=pass"}
        psql "$DB_CONN" -c "$sql" >/dev/null 2>&1 || true
        redis-cli FLUSHALL >/dev/null 2>&1 || true
    fi
}

# Helper to verify HTTP status code, body, and pass authorization headers
# Args: method, url_path, expected_status, data (optional), token (optional)
assert_request() {
    local method=$1
    local path=$2
    local expected_status=$3
    local data=$4
    local token=$5
    local status_code
    local response_body
    local temp_res
    temp_res=$(mktemp)

    echo "Running: $method $path"

    local headers=("-H" "Content-Type: application/json")
    if [ -n "$token" ]; then
        headers+=("-H" "Authorization: Bearer $token")
    fi

    if [ -n "$data" ]; then
        status_code=$(curl -s -w "%{http_code}" -o "$temp_res" -X "$method" -d "$data" "${headers[@]}" "$SERVER_URL$path")
    else
        status_code=$(curl -s -w "%{http_code}" -o "$temp_res" -X "$method" "${headers[@]}" "$SERVER_URL$path")
    fi

    response_body=$(cat "$temp_res")
    rm -f "$temp_res"

    if [ "$status_code" -ne "$expected_status" ]; then
        echo "Assertion FAILED: Expected status $expected_status, got $status_code"
        echo "Response body: $response_body"
        exit 1
    fi

    echo "Response: $response_body"
    echo "Status code: $status_code"
    echo "---"
}
