#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Running WebSocket Real-time Collaboration Tests ==="

user_a_email="ws_user_a_$RANDOM@example.com"
user_b_email="ws_user_b_$RANDOM@example.com"
user_c_email="ws_user_c_$RANDOM@example.com"
password="Password123!"

assert_request "POST" "/signup" 201 "{\"email\":\"$user_a_email\",\"password\":\"$password\"}"
assert_request "POST" "/signup" 201 "{\"email\":\"$user_b_email\",\"password\":\"$password\"}"
assert_request "POST" "/signup" 201 "{\"email\":\"$user_c_email\",\"password\":\"$password\"}"

login_a=$(curl -s -X POST -H "Content-Type: application/json" -d "{\"email\":\"$user_a_email\",\"password\":\"$password\"}" "$SERVER_URL/login")
token_a=$(echo "$login_a" | jq -r '.token')

login_b=$(curl -s -X POST -H "Content-Type: application/json" -d "{\"email\":\"$user_b_email\",\"password\":\"$password\"}" "$SERVER_URL/login")
token_b=$(echo "$login_b" | jq -r '.token')

login_c=$(curl -s -X POST -H "Content-Type: application/json" -d "{\"email\":\"$user_c_email\",\"password\":\"$password\"}" "$SERVER_URL/login")
token_c=$(echo "$login_c" | jq -r '.token')

echo "Tokens acquired for User A, User B, User C."

# User A creates a note
create_res=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $token_a" -d '{"title":"WS Collaboration Note","content":"Initial text"}' "$SERVER_URL/notes")
note_id=$(echo "$create_res" | jq -r '.id')

if [ -z "$note_id" ] || [ "$note_id" == "null" ]; then
    echo "Assertion FAILED: Note creation failed. Response: $create_res"
    exit 1
fi
echo "Created note ID: $note_id"

# User A shares note with User B
share_res=$(curl -s -w "%{http_code}" -o /dev/null -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $token_a" -d "{\"target_email\":\"$user_b_email\",\"permission\":\"editor\"}" "$SERVER_URL/notes/$note_id/share")
if [ "$share_res" -ne 200 ]; then
    echo "Assertion FAILED: Failed to share note with User B, got $share_res"
    exit 1
fi
echo "Shared note with User B as editor."

# Execute WebSocket interactive collaboration test script
python3 "$SCRIPT_DIR/../scripts/websocket_collaboration.py" \
    --server-url "$SERVER_URL" \
    --note-id "$note_id" \
    --token-a "$token_a" \
    --token-b "$token_b" \
    --token-c "$token_c" \
    --email-a "$user_a_email" \
    --email-b "$user_b_email"

echo "=== WebSocket Real-time Collaboration Tests Passed Successfully! ==="
