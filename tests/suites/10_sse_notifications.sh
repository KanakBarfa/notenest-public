#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Running SSE & Real-time Notifications Tests ==="

# 1. Create User A and User B
user_a_email="sse_user_a_$RANDOM@example.com"
user_b_email="sse_user_b_$RANDOM@example.com"
password="Password123!"

assert_request "POST" "/signup" 201 "{\"email\":\"$user_a_email\",\"password\":\"$password\"}"
assert_request "POST" "/signup" 201 "{\"email\":\"$user_b_email\",\"password\":\"$password\"}"

login_a=$(curl -s -X POST -H "Content-Type: application/json" -d "{\"email\":\"$user_a_email\",\"password\":\"$password\"}" "$SERVER_URL/login")
token_a=$(echo "$login_a" | jq -r '.token')

login_b=$(curl -s -X POST -H "Content-Type: application/json" -d "{\"email\":\"$user_b_email\",\"password\":\"$password\"}" "$SERVER_URL/login")
token_b=$(echo "$login_b" | jq -r '.token')

if [ -z "$token_a" ] || [ "$token_a" == "null" ] || [ -z "$token_b" ] || [ "$token_b" == "null" ]; then
    echo "Assertion FAILED: Login failed for test users"
    exit 1
fi

echo "User A token acquired."
echo "User B token acquired."

# 2. Start SSE listener for User B in background
sse_out=$(mktemp)
curl -s -N "$SERVER_URL/events?token=$token_b" > "$sse_out" 2>&1 &
SSE_PID=$!

cleanup_sse() {
    kill "$SSE_PID" 2>/dev/null || true
    rm -f "$sse_out"
}
trap cleanup_sse EXIT

# Give SSE stream connection time to establish
sleep 1

# 3. User A creates a note
create_res=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $token_a" -d '{"title":"SSE Shared Note Title","content":"Hello SSE world!"}' "$SERVER_URL/notes")
note_id=$(echo "$create_res" | jq -r '.id')

if [ -z "$note_id" ] || [ "$note_id" == "null" ]; then
    echo "Assertion FAILED: Failed to create note for User A"
    exit 1
fi

echo "Created note ID: $note_id"

# 4. User A shares note with User B
share_res=$(curl -s -w "%{http_code}" -o /tmp/share_res.json -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $token_a" -d "{\"target_email\":\"$user_b_email\"}" "$SERVER_URL/notes/$note_id/share")

if [ "$share_res" -ne 200 ]; then
    echo "Assertion FAILED: Expected 200 on share note, got $share_res"
    cat /tmp/share_res.json
    exit 1
fi

echo "Share response 200 OK verified."

# 5. Wait for SSE event to arrive on User B's stream
received=false
for i in {1..20}; do
    if grep -q "note_shared" "$sse_out" 2>/dev/null; then
        received=true
        break
    fi
    sleep 0.2
done

if ! $received; then
    echo "Assertion FAILED: User B did not receive SSE note_shared notification event within timeout"
    echo "Current SSE output log:"
    cat "$sse_out"
    exit 1
fi

echo "SSE Notification Event received by User B!"

# 6. Verify SSE payload fields
if ! grep -q "$note_id" "$sse_out"; then
    echo "Assertion FAILED: SSE payload missing note ID $note_id"
    cat "$sse_out"
    exit 1
fi

if ! grep -q "$user_a_email" "$sse_out"; then
    echo "Assertion FAILED: SSE payload missing sender email $user_a_email"
    cat "$sse_out"
    exit 1
fi

# 7. User B fetches notes list to verify shared note appears
user_b_notes=$(curl -s -H "Authorization: Bearer $token_b" "$SERVER_URL/notes")
if ! echo "$user_b_notes" | grep -q "$note_id"; then
    echo "Assertion FAILED: Shared note $note_id not found in User B notes list"
    echo "User B notes: $user_b_notes"
    exit 1
fi
echo "Shared note access verified in User B notes list!"

# 8. Test Viewer vs Editor Permission Enforcement
user_c_email="sse_user_c_$RANDOM@example.com"
signup_c=$(curl -s -X POST -H "Content-Type: application/json" -d "{\"email\":\"$user_c_email\",\"password\":\"$password\"}" "$SERVER_URL/signup")
user_c_id=$(echo "$signup_c" | jq -r '.user_id')
login_c=$(curl -s -X POST -H "Content-Type: application/json" -d "{\"email\":\"$user_c_email\",\"password\":\"$password\"}" "$SERVER_URL/login")
token_c=$(echo "$login_c" | jq -r '.token')

# Share with User C as viewer
share_viewer_res=$(curl -s -w "%{http_code}" -o /tmp/share_v.json -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $token_a" -d "{\"target_email\":\"$user_c_email\",\"permission\":\"viewer\"}" "$SERVER_URL/notes/$note_id/share")
if [ "$share_viewer_res" -ne 200 ]; then
    echo "Assertion FAILED: Expected 200 on share viewer note, got $share_viewer_res"
    exit 1
fi

# User C attempts edit -> Expect HTTP 403 Forbidden
edit_viewer_status=$(curl -s -w "%{http_code}" -o /dev/null -X PUT -H "Content-Type: application/json" -H "Authorization: Bearer $token_c" -d '{"title":"Viewer Edit Attempt","content":"Should fail"}' "$SERVER_URL/notes/$note_id")
if [ "$edit_viewer_status" -ne 403 ]; then
    echo "Assertion FAILED: Expected 403 Forbidden on viewer edit attempt, got $edit_viewer_status"
    exit 1
fi
echo "Viewer read-only 403 Forbidden enforcement verified!"

# Upgrade User C to editor
share_editor_res=$(curl -s -w "%{http_code}" -o /dev/null -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $token_a" -d "{\"target_email\":\"$user_c_email\",\"permission\":\"editor\"}" "$SERVER_URL/notes/$note_id/share")
if [ "$share_editor_res" -ne 200 ]; then
    echo "Assertion FAILED: Expected 200 on share editor upgrade, got $share_editor_res"
    exit 1
fi

# User C attempts edit -> Expect HTTP 200 OK
edit_editor_status=$(curl -s -w "%{http_code}" -o /dev/null -X PUT -H "Content-Type: application/json" -H "Authorization: Bearer $token_c" -d '{"title":"Editor Edit Success","content":"Should succeed"}' "$SERVER_URL/notes/$note_id")
if [ "$edit_editor_status" -ne 200 ]; then
    echo "Assertion FAILED: Expected 200 OK on editor edit attempt, got $edit_editor_status"
    exit 1
fi
echo "Editor permission upgrade & edit 200 OK verified!"

# List note shares
shares_list=$(curl -s -H "Authorization: Bearer $token_a" "$SERVER_URL/notes/$note_id/shares")
if ! echo "$shares_list" | grep -q "$user_c_email"; then
    echo "Assertion FAILED: Shares list missing $user_c_email"
    echo "Shares list: $shares_list"
    exit 1
fi
echo "GET /notes/$note_id/shares verified!"

# Revoke User C share
revoke_status=$(curl -s -w "%{http_code}" -o /dev/null -X DELETE -H "Authorization: Bearer $token_a" "$SERVER_URL/notes/$note_id/shares/$user_c_id")
if [ "$revoke_status" -ne 204 ]; then
    echo "Assertion FAILED: Expected 204 on revoke share, got $revoke_status"
    exit 1
fi
echo "DELETE /notes/$note_id/shares/$user_c_id revoked successfully!"

echo "=== SSE & Real-time Notifications Tests Passed Successfully! ==="
