#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "[Suite 30] Read-your-writes consistency (primary login, sticky GraphQL reads)..."
register_cleanup

GRAPHQL_URL="http://localhost:4000/graphql"

# 1. Signup followed IMMEDIATELY by login must succeed: login hits the
#    primary, so replica lag can never reject a first login.
email="ryw_$RANDOM@example.com"
assert_request "POST" "/signup" 201 "{\"email\":\"$email\",\"password\":\"Password123!\"}"
login_res=$(curl -s -X POST "$SERVER_URL/login" -H "Content-Type: application/json" \
    -d "{\"email\":\"$email\",\"password\":\"Password123!\"}")
token=$(echo "$login_res" | jq -r '.token')
if [ -z "$token" ] || [ "$token" = "null" ]; then
    echo "FAILED: immediate post-signup login rejected: $login_res"
    exit 1
fi
echo "--> Immediate login after signup succeeded (primary-side auth)."

# 2. REST write then instant REST read shows the note.
note_res=$(curl -s -X POST "$SERVER_URL/notes" -H "Authorization: Bearer $token" \
    -H "Content-Type: application/json" -d '{"title":"RYW Note","content":"fresh"}')
note_id=$(echo "$note_res" | jq -r '.id')
[ -n "$note_id" ] && [ "$note_id" != "null" ] || { echo "FAILED: note creation"; exit 1; }

list_res=$(curl -s "$SERVER_URL/notes" -H "Authorization: Bearer $token")
echo "$list_res" | grep -q "$note_id" || {
    echo "FAILED: just-written note invisible to immediate REST read"; exit 1;
}
echo "--> REST read-after-write consistent."

# 3. GraphQL mutation then instant query sees the new note (sticky window).
gql_create=$(curl -s -X POST "$GRAPHQL_URL" -H "Authorization: Bearer $token" \
    -H "Content-Type: application/json" \
    -d '{"query":"mutation { createNote(title: \"RYW GQL\", content: \"x\") { id } }"}')
gql_note_id=$(echo "$gql_create" | jq -r '.data.createNote.id // empty')
[ -n "$gql_note_id" ] || { echo "FAILED: graphql create:"; echo "$gql_create"; exit 1; }

gql_read=$(curl -s -X POST "$GRAPHQL_URL" -H "Authorization: Bearer $token" \
    -H "Content-Type: application/json" \
    -d "{\"query\":\"{ notes(limit: 100) { id title } }\"}")
echo "$gql_read" | grep -q "$gql_note_id" || {
    echo "FAILED: GraphQL query right after mutation missed the new note:"
    echo "$gql_read"
    exit 1;
}
echo "--> GraphQL read-after-mutation served fresh data (sticky window)."

# 4. Comment written via GraphQL is immediately visible in the comments query.
payload=$(jq -n --arg q "mutation { createComment(noteId: \"$gql_note_id\", content: \"first\") { id } }" '{query: $q}')
comment_res=$(curl -s -X POST "$GRAPHQL_URL" -H "Authorization: Bearer $token" \
    -H "Content-Type: application/json" -d "$payload")
comment_id=$(echo "$comment_res" | jq -r '.data.createComment.id // empty')
[ -n "$comment_id" ] || { echo "FAILED: comment create:"; echo "$comment_res"; exit 1; }

payload=$(jq -n --arg q "{ comments(noteId: \"$gql_note_id\") { id } }" '{query: $q}')
comments_read=$(curl -s -X POST "$GRAPHQL_URL" -H "Authorization: Bearer $token" \
    -H "Content-Type: application/json" -d "$payload")
echo "$comments_read" | grep -q "$comment_id" || {
    echo "FAILED: fresh comment missing from immediate query"; echo "$comments_read"; exit 1;
}
echo "--> Comments read-your-writes verified."

echo "[Suite 30] PASSED"
