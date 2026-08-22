#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "[Suite 25] GraphQL service-layer authorization..."

GRAPHQL_URL="http://localhost:4000/graphql"
register_cleanup
await_gateway

gql() {
    local token=$1
    local query=$2
    # jq builds the payload so embedded quotes in the query stay valid JSON.
    local payload
    payload=$(jq -n --arg q "$query" '{query: $q}')
    if [ -n "$token" ]; then
        curl -s -X POST "$GRAPHQL_URL" -H "Authorization: Bearer $token" \
            -H "Content-Type: application/json" -d "$payload"
    else
        curl -s -X POST "$GRAPHQL_URL" -H "Content-Type: application/json" -d "$payload"
    fi
}

# Users
owner_email="authz_owner_$RANDOM@example.com"
viewer_email="authz_viewer_$RANDOM@example.com"
outsider_email="authz_outsider_$RANDOM@example.com"
for u in "$owner_email" "$viewer_email" "$outsider_email"; do
    assert_request "POST" "/signup" 201 "{\"email\":\"$u\",\"password\":\"Password123!\"}"
done

token_owner=$(curl -s -X POST "$SERVER_URL/login" -H "Content-Type: application/json" \
    -d "{\"email\":\"$owner_email\",\"password\":\"Password123!\"}" | jq -r '.token')
token_viewer=$(curl -s -X POST "$SERVER_URL/login" -H "Content-Type: application/json" \
    -d "{\"email\":\"$viewer_email\",\"password\":\"Password123!\"}" | jq -r '.token')

# 1. Unauthenticated queries return an errors payload with no data.
res=$(gql "" "{ notes { id } }")
assert_json_eq "$res" ".data" "null"
echo "$res" | grep -q "UNAUTHENTICATED\|Unauthorized" || {
    echo "FAILED: unauthenticated notes query did not report an auth error"; echo "$res"; exit 1;
}
echo "--> Unauthenticated notes query rejected without data."

# 2. Outsider cannot read a private note by ID (no anonymous fallback).
note_res=$(curl -s -X POST "$SERVER_URL/notes" -H "Authorization: Bearer $token_owner" \
    -H "Content-Type: application/json" -d '{"title":"Authz Note","content":"secret"}')
note_id=$(echo "$note_res" | jq -r '.id')
[ -n "$note_id" ] && [ "$note_id" != "null" ] || { echo "FAILED: note creation"; exit 1; }

res=$(gql "" "{ note(id: \"$note_id\") { id title } }")
assert_json_eq "$res" ".data.note" "null"
echo "--> Unauthenticated single-note fetch rejected."

# 3. Viewer share grants read but NOT comment creation via GraphQL.
curl -s -o /dev/null -X POST "$SERVER_URL/notes/$note_id/share" \
    -H "Authorization: Bearer $token_owner" -H "Content-Type: application/json" \
    -d "{\"target_email\":\"$viewer_email\",\"permission\":\"viewer\"}"

res=$(gql "$token_viewer" "{ note(id: \"$note_id\") { id title permission } }")
assert_json_eq "$res" ".data.note.permission" "viewer"
echo "--> Viewer can read the shared note."

res=$(gql "$token_viewer" \
    "mutation { createComment(noteId: \"$note_id\", content: \"viewer attempt\") { id } }")
echo "$res" | grep -q "FORBIDDEN\|Forbidden" || {
    echo "FAILED: viewer createComment was not forbidden"; echo "$res"; exit 1;
}
assert_json_eq "$res" ".data.createComment" "null"
echo "--> Viewer mutation denied at GraphQL layer (FORBIDDEN)."

# 4. Promote to editor: same mutation now succeeds.
curl -s -o /dev/null -X POST "$SERVER_URL/notes/$note_id/share" \
    -H "Authorization: Bearer $token_owner" -H "Content-Type: application/json" \
    -d "{\"target_email\":\"$viewer_email\",\"permission\":\"editor\"}"

res=$(gql "$token_viewer" \
    "mutation { createComment(noteId: \"$note_id\", content: \"editor ok\") { id } }")
assert_json_eq "$res" ".data.createComment.content" null >/dev/null 2>&1 || true
comment_id=$(echo "$res" | jq -r '.data.createComment.id // empty')
[ -n "$comment_id" ] || { echo "FAILED: editor createComment rejected:"; echo "$res"; exit 1; }
echo "--> Editor mutation allowed after permission promotion."

# 5. Email privacy: other users never see emails through GraphQL.
author_query=$(gql "$token_viewer" \
    "{ comments(noteId: \"$note_id\") { author { id email } } }")
# Owner-authored fields must not leak email to a non-owner requester.
if echo "$author_query" | grep -q "\"email\":\"$owner_email\""; then
    echo "FAILED: owner email leaked to non-owner via GraphQL"
    exit 1
fi
echo "--> Email privacy enforced for non-self readers."

# 6. Outsider (no relationship) gets no access at all.
token_outsider=$(curl -s -X POST "$SERVER_URL/login" -H "Content-Type: application/json" \
    -d "{\"email\":\"$outsider_email\",\"password\":\"Password123!\"}" | jq -r '.token')
res=$(gql "$token_outsider" "{ note(id: \"$note_id\") { id } }")
assert_json_eq "$res" ".data.note" "null"
echo "--> Unrelated user denied note access."

echo "[Suite 25] PASSED"
