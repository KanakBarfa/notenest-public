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

# Asserts a JSON document contains an exact value at a top-level jq path.
# Args: json_text, jq_path, expected_value
assert_json_eq() {
    local json_text=$1
    local jq_path=$2
    local expected=$3
    local actual
    actual=$(echo "$json_text" | jq -r "$jq_path")
    if [ "$actual" != "$expected" ]; then
        echo "Assertion FAILED: $jq_path expected '$expected', got '$actual'"
        echo "JSON: $json_text"
        exit 1
    fi
}

# Asserts a string contains a substring; fails otherwise.
assert_contains() {
    local haystack=$1
    local needle=$2
    case "$haystack" in
        *"$needle"*) return 0 ;;
        *)
            echo "Assertion FAILED: expected to find '$needle' in: ${haystack:0:300}"
            exit 1
            ;;
    esac
}

# Registers the current suite for guaranteed cleanup on any exit.
register_cleanup() {
    trap 'cleanup_db' EXIT
}

# Waits until the full gateway chain (HAProxy -> Kong -> app) serves health.
await_gateway() {
    local code
    for attempt in {1..30}; do
        code=$(curl -s -o /dev/null -w "%{http_code}" "$SERVER_URL/health" || echo 000)
        if [ "$code" -eq 200 ]; then
            return 0
        fi
        sleep 2
    done
    echo "FAILED: gateway did not become healthy within 60s"
    exit 1
}
