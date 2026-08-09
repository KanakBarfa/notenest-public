#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Running Cache Validation Tests ==="

curl -s -X POST -d '{"email":"user_cache@example.com","password":"password123"}' -H "Content-Type: application/json" "$SERVER_URL/signup" >/dev/null || true
token_c=$(curl -s -X POST -d '{"email":"user_cache@example.com","password":"password123"}' -H "Content-Type: application/json" "$SERVER_URL/login" | jq -r '.token')

if [ -z "$token_c" ] || [ "$token_c" == "null" ]; then
    echo "Failed to acquire token for Cache test."
    exit 1
fi

# Get notes (first time, miss)
assert_request "GET" "/notes" 200 "" "$token_c"

# Get notes (second time, hit)
assert_request "GET" "/notes" 200 "" "$token_c"

# Create a new note (invalidates cache list)
note_c_data='{"title":"Cache Note","content":"Testing cache"}'
note_c_res=$(curl -s -X POST -d "$note_c_data" -H "Content-Type: application/json" -H "Authorization: Bearer $token_c" "$SERVER_URL/notes")
note_c_id=$(echo "$note_c_res" | jq -r '.id')
if [ -z "$note_c_id" ] || [ "$note_c_id" == "null" ]; then
    echo "Failed to create Note for cache test: $note_c_res"
    exit 1
fi

# Get specific note (first time, miss)
assert_request "GET" "/notes/$note_c_id" 200 "" "$token_c"

# Get specific note (second time, hit)
assert_request "GET" "/notes/$note_c_id" 200 "" "$token_c"

# Update note (invalidates cache)
note_c_update='{"title":"Updated Cache Note","content":"Updated content"}'
assert_request "PUT" "/notes/$note_c_id" 200 "$note_c_update" "$token_c"

# Get note after update (miss then hit)
assert_request "GET" "/notes/$note_c_id" 200 "" "$token_c"
assert_request "GET" "/notes/$note_c_id" 200 "" "$token_c"

# Delete note (invalidates cache)
assert_request "DELETE" "/notes/$note_c_id" 204 "" "$token_c"

echo "Cache validation tests passed!"
