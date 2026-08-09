#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Running Note Isolation Tests ==="

user_a_res=$(curl -s -X POST -d '{"email":"usera_iso@example.com","password":"password123"}' -H "Content-Type: application/json" "$SERVER_URL/signup")
token_a=$(curl -s -X POST -d '{"email":"usera_iso@example.com","password":"password123"}' -H "Content-Type: application/json" "$SERVER_URL/login" | jq -r '.token')

user_b_res=$(curl -s -X POST -d '{"email":"userb_iso@example.com","password":"password456"}' -H "Content-Type: application/json" "$SERVER_URL/signup")
token_b=$(curl -s -X POST -d '{"email":"userb_iso@example.com","password":"password456"}' -H "Content-Type: application/json" "$SERVER_URL/login" | jq -r '.token')

if [ -z "$token_a" ] || [ "$token_a" == "null" ] || [ -z "$token_b" ] || [ "$token_b" == "null" ]; then
    echo "Failed to acquire tokens for Note Isolation test."
    exit 1
fi

note_a1_data='{"title":"Note A1","content":"Content of note A1"}'
note_a1_res=$(curl -s -X POST -d "$note_a1_data" -H "Content-Type: application/json" -H "Authorization: Bearer $token_a" "$SERVER_URL/notes")
note_a1_id=$(echo "$note_a1_res" | jq -r '.id')

note_a2_data='{"title":"Note A2","content":"Content of note A2"}'
note_a2_res=$(curl -s -X POST -d "$note_a2_data" -H "Content-Type: application/json" -H "Authorization: Bearer $token_a" "$SERVER_URL/notes")
note_a2_id=$(echo "$note_a2_res" | jq -r '.id')

note_b1_data='{"title":"Note B1","content":"Content of note B1"}'
note_b1_res=$(curl -s -X POST -d "$note_b1_data" -H "Content-Type: application/json" -H "Authorization: Bearer $token_b" "$SERVER_URL/notes")
note_b1_id=$(echo "$note_b1_res" | jq -r '.id')

echo "Fetching all notes for User A..."
user_a_notes=$(curl -s -H "Authorization: Bearer $token_a" "$SERVER_URL/notes")
user_a_notes_count=$(echo "$user_a_notes" | jq '. | length')
if [ "$user_a_notes_count" -ne 2 ]; then
    echo "Assertion FAILED: Expected 2 notes for User A, got $user_a_notes_count"
    exit 1
fi
echo "User A sees exactly $user_a_notes_count notes (correct)."

echo "Fetching all notes for User B..."
user_b_notes=$(curl -s -H "Authorization: Bearer $token_b" "$SERVER_URL/notes")
user_b_notes_count=$(echo "$user_b_notes" | jq '. | length')
if [ "$user_b_notes_count" -ne 1 ]; then
    echo "Assertion FAILED: Expected 1 note for User B, got $user_b_notes_count"
    exit 1
fi
echo "User B sees exactly $user_b_notes_count notes (correct)."

assert_request "GET" "/notes/$note_a1_id" 404 "" "$token_b"
update_data='{"title":"Hacked Note","content":"I updated this note."}'
assert_request "PUT" "/notes/$note_a1_id" 404 "$update_data" "$token_b"
assert_request "DELETE" "/notes/$note_a1_id" 404 "" "$token_b"

assert_request "GET" "/notes/$note_a1_id" 200 "" "$token_a"
user_a_update='{"title":"Updated Note A1","content":"New content for A1"}'
assert_request "PUT" "/notes/$note_a1_id" 200 "$user_a_update" "$token_a"

# Test: 100k word limit per note
large_file=$(mktemp)
python3 "$SCRIPT_DIR/../scripts/generate_large_note.py" > "$large_file"
assert_request "POST" "/notes" 400 "@$large_file" "$token_a"
rm -f "$large_file"
echo "Verified 100k word limit per note (returned status 400)."

assert_request "DELETE" "/notes/$note_a1_id" 204 "" "$token_a"
assert_request "GET" "/notes/$note_a1_id" 404 "" "$token_a"

echo "Note isolation tests passed!"
