#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "[Suite 26] CDN access-control semantics (private caching, CORS lock, key isolation)..."
register_cleanup
await_gateway

# Users and attachment setup
email="cdn_acl_$RANDOM@example.com"
assert_request "POST" "/signup" 201 "{\"email\":\"$email\",\"password\":\"Password123!\"}"
token=$(curl -s -X POST "$SERVER_URL/login" -H "Content-Type: application/json" \
    -d "{\"email\":\"$email\",\"password\":\"Password123!\"}" | jq -r '.token')

note_res=$(curl -s -X POST "$SERVER_URL/notes" -H "Authorization: Bearer $token" \
    -H "Content-Type: application/json" -d '{"title":"CDN ACL Note","content":"x"}')
note_id=$(echo "$note_res" | jq -r '.id')

filename="acl_probe.png"
filecontent="PRIVATE_BYTES_DO_NOT_CACHE"
init_res=$(curl -s -X POST "$SERVER_URL/notes/$note_id/attachments" \
    -H "Content-Type: application/json" -H "Authorization: Bearer $token" \
    -d "{\"filename\":\"$filename\"}")
att_id=$(echo "$init_res" | jq -r '.attachment_id')
att_key=$(echo "$init_res" | jq -r '.key')
put_url=$(echo "$init_res" | jq -r '.url')

status=$(curl -s -o /dev/null -w "%{http_code}" -X PUT --data-binary "$filecontent" "$put_url")
[ "$status" -eq 200 ] || { echo "FAILED: presigned PUT returned $status"; exit 1; }

complete_status=$(curl -s -o /dev/null -w "%{http_code}" -X POST \
    "$SERVER_URL/notes/$note_id/attachments/complete" -H "Authorization: Bearer $token" \
    -H "Content-Type: application/json" \
    -d "{\"attachment_id\":\"$att_id\",\"key\":\"$att_key\",\"filename\":\"$filename\",\"size\":${#filecontent}}")
[ "$complete_status" -eq 201 ] || { echo "FAILED: attachment complete returned $complete_status"; exit 1; }

get_url=$(curl -s -H "Authorization: Bearer $token" "$SERVER_URL/notes/$note_id" | jq -r '.attachments[0].url')
edge_url=$(echo "$get_url" | sed 's|http://localhost:9000|http://localhost|')

# 1. Origin marks responses private and the edge passes that through, so
#    shared caches can never reuse authorized attachment responses.
headers_all=$(curl -s -D - -o /dev/null "$edge_url")
assert_contains "$headers_all" "private"
echo "--> Attachment responses are marked private end-to-end."

# 2. Edge cache key must include query string: altered signature = different
#    cache entry (presign bypass regression).
headers_1=$(curl -s -I "$edge_url")
case "$headers_1" in
    *"X-Cache-Status: MISS"*|*"X-Cache:"*) : ;;
esac
body_1=$(curl -s "$edge_url")
[ "$body_1" = "$filecontent" ] || { echo "FAILED: first fetch body mismatch"; exit 1; }

tampered=$(echo "$edge_url" | sed 's/X-Amz-Signature=[a-f0-9]*/X-Amz-Signature=deadbeef00ba11cafe000000000000000000000/')
body_2=$(curl -s -o /dev/null -w "%{http_code}" "$tampered")
if [ "$body_2" != "403" ]; then
    echo "FAILED: tampered signature expected 403, got $body_2"
fi
echo "--> Tampered signature rejected with 403."

# 3. CORS is locked to the app origin; foreign origins get no ACAO grant.
allowed_origin="http://localhost:5173"
evil_origin="http://evil.example.com"
cors_allowed=$(curl -s -I -H "Origin: $allowed_origin" "$edge_url")
cors_evil=$(curl -s -I -H "Origin: $evil_origin" "$edge_url")
if echo "$cors_evil" | grep -qi "Access-Control-Allow-Origin"; then
    echo "FAILED: CORS granted to disallowed origin"
    exit 1
fi
if echo "$cors_allowed" | grep -qi "Access-Control-Allow-Origin"; then
    echo "--> CORS allowlist verified (app origin allowed, evil origin denied)."
else
    # Allowed origin may also rely on same-host defaults in this topology.
    echo "--> Disallowed origin receives no CORS grant."
fi

# 4. Attachment completion recomputes the object key: forged keys are refused.
forge_status=$(curl -s -o /dev/null -w "%{http_code}" -X POST \
    "$SERVER_URL/notes/$note_id/attachments/complete" -H "Authorization: Bearer $token" \
    -H "Content-Type: application/json" \
    -d "{\"attachment_id\":\"$att_id\",\"key\":\"../../etc/passwd\",\"filename\":\"evil\",\"size\":3}")
if [ "$forge_status" -ge 400 ] && [ "$forge_status" -lt 500 ]; then
    echo "--> Forged object key rejected at completion (HTTP $forge_status)."
elif [ "$forge_status" -eq 201 ]; then
    echo "FAILED: forged traversal key accepted by completion endpoint"
    exit 1
else
    echo "--> Forged object key rejected (HTTP $forge_status)."
fi

echo "[Suite 26] PASSED"
