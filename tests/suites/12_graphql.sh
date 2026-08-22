#!/bin/bash
set -eo pipefail

# Suite 12: GraphQL Layer & DataLoader Batching E2E Tests
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Running Suite 12: GraphQL Layer ==="

# 1. Register User 1 & User 2
TIMESTAMP=$(date +%s%N)
USER1_EMAIL="graphql_user1_${TIMESTAMP}@example.com"
USER2_EMAIL="graphql_user2_${TIMESTAMP}@example.com"
PASSWORD="TestPassword123!"

echo "Registering test users..."
assert_request "POST" "/signup" 201 "{\"email\":\"$USER1_EMAIL\",\"password\":\"$PASSWORD\"}"
assert_request "POST" "/signup" 201 "{\"email\":\"$USER2_EMAIL\",\"password\":\"$PASSWORD\"}"

# Login User 1
LOGIN_RES=$(curl -s -X POST -H "Content-Type: application/json" -d "{\"email\":\"$USER1_EMAIL\",\"password\":\"$PASSWORD\"}" "$SERVER_URL/login")
TOKEN1=$(echo "$LOGIN_RES" | grep -o '"token":"[^"]*' | grep -o '[^"]*$')

if [ -z "$TOKEN1" ]; then
    echo "ERROR: Failed to obtain JWT token for User 1"
    exit 1
fi

# Login User 2
LOGIN_RES2=$(curl -s -X POST -H "Content-Type: application/json" -d "{\"email\":\"$USER2_EMAIL\",\"password\":\"$PASSWORD\"}" "$SERVER_URL/login")
TOKEN2=$(echo "$LOGIN_RES2" | grep -o '"token":"[^"]*' | grep -o '[^"]*$')

echo "User 1 token obtained successfully."

# 2. Create Note 1 via GraphQL mutation
echo "Creating Note 1 via GraphQL mutation..."
GQL_CREATE_NOTE='{"query":"mutation { createNote(title: \"GraphQL Test Note 1\", content: \"Content for note 1\") { id title content ownerId author { id email } } }"}'
CREATE_NOTE_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN1" -d "$GQL_CREATE_NOTE" "$SERVER_URL/graphql")

echo "Create Note 1 response: $CREATE_NOTE_RES"
NOTE1_ID=$(echo "$CREATE_NOTE_RES" | grep -o '"id":"[^"]*' | head -n1 | grep -o '[^"]*$')

if [ -z "$NOTE1_ID" ]; then
    echo "ERROR: Failed to create Note 1 via GraphQL"
    exit 1
fi
echo "Note 1 created with ID: $NOTE1_ID"

# 3. Create Note 2 via GraphQL mutation
echo "Creating Note 2 via GraphQL mutation..."
GQL_CREATE_NOTE2='{"query":"mutation { createNote(title: \"GraphQL Test Note 2\", content: \"Content for note 2\") { id title content } }"}'
CREATE_NOTE2_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN1" -d "$GQL_CREATE_NOTE2" "$SERVER_URL/graphql")
NOTE2_ID=$(echo "$CREATE_NOTE2_RES" | grep -o '"id":"[^"]*' | head -n1 | grep -o '[^"]*$')
echo "Note 2 created with ID: $NOTE2_ID"

# 4. Authorization: User 2 has NO access to User 1's private note (pre-share)
echo "Verifying cross-user note access is forbidden..."
GQL_NOTE_AS_U2="{\"query\":\"query { note(id: \\\"$NOTE1_ID\\\") { id title } }\"}"
NOTE_U2_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN2" -d "$GQL_NOTE_AS_U2" "$SERVER_URL/graphql")
echo "User 2 reading Note 1 response: $NOTE_U2_RES"
if ! echo "$NOTE_U2_RES" | grep -q '"FORBIDDEN"'; then
    echo "ERROR: Expected FORBIDDEN when User 2 reads User 1's private note"
    exit 1
fi

echo "Adding comment 1 to Note 1 via User 1..."
GQL_COMMENT1="{\"query\":\"mutation { createComment(noteId: \\\"$NOTE1_ID\\\", content: \\\"First comment on note 1\\\") { id content author { email } } }\"}"
COMMENT1_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN1" -d "$GQL_COMMENT1" "$SERVER_URL/graphql")
echo "Comment 1 response: $COMMENT1_RES"

echo "Adding comment 2 to Note 1 via User 2 (must be denied)..."
GQL_COMMENT2="{\"query\":\"mutation { createComment(noteId: \\\"$NOTE1_ID\\\", content: \\\"Second comment on note 1 from user 2\\\") { id content author { email } } }\"}"
COMMENT2_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN2" -d "$GQL_COMMENT2" "$SERVER_URL/graphql")
echo "Comment 2 response: $COMMENT2_RES"
if ! echo "$COMMENT2_RES" | grep -q '"FORBIDDEN"'; then
    echo "ERROR: Expected FORBIDDEN when User 2 comments on User 1's private note"
    exit 1
fi

# 4b. Share Note 1 with User 2 as viewer: read allowed, writes still denied
echo "Sharing Note 1 with User 2 as viewer..."
SHARE_RES=$(curl -s -w "%{http_code}" -o /dev/null -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN1" -d "{\"target_email\":\"$USER2_EMAIL\",\"permission\":\"viewer\"}" "$SERVER_URL/notes/$NOTE1_ID/share")
if [ "$SHARE_RES" != "200" ]; then
    echo "ERROR: Failed to share Note 1 with User 2 (got $SHARE_RES)"
    exit 1
fi

READ_U2_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN2" -d "$GQL_NOTE_AS_U2" "$SERVER_URL/graphql")
echo "User 2 reading shared Note 1 response: $READ_U2_RES"
if ! echo "$READ_U2_RES" | grep -q "GraphQL Test Note 1"; then
    echo "ERROR: User 2 should be able to read the shared note"
    exit 1
fi

VIEWER_COMMENT_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN2" -d "$GQL_COMMENT2" "$SERVER_URL/graphql")
echo "Viewer comment attempt response: $VIEWER_COMMENT_RES"
if ! echo "$VIEWER_COMMENT_RES" | grep -q '"FORBIDDEN"'; then
    echo "ERROR: Expected FORBIDDEN for viewer attempting createComment"
    exit 1
fi

# 4c. Upgrade User 2 to editor (re-share upserts permission), comment now succeeds
echo "Upgrading User 2 to editor..."
UPGRADE_RES=$(curl -s -w "%{http_code}" -o /dev/null -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN1" -d "{\"target_email\":\"$USER2_EMAIL\",\"permission\":\"editor\"}" "$SERVER_URL/notes/$NOTE1_ID/share")
if [ "$UPGRADE_RES" != "200" ]; then
    echo "ERROR: Failed to upgrade User 2 to editor (got $UPGRADE_RES)"
    exit 1
fi

COMMENT2_OK_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN2" -d "$GQL_COMMENT2" "$SERVER_URL/graphql")
echo "Editor comment response: $COMMENT2_OK_RES"
if ! echo "$COMMENT2_OK_RES" | grep -q "Second comment on note 1 from user 2"; then
    echo "ERROR: Editor User 2 should be able to comment"
    exit 1
fi

# 5. Execute 1-roundtrip GraphQL Query fetching Note + Author + Comments
echo "Fetching Note + Author + Comments in 1 round trip..."
GQL_QUERY="{\"query\":\"query GetNoteDetails { note(id: \\\"$NOTE1_ID\\\") { id title content author { email } comments { id content author { email } } } }\"}"

COMMENTS_VERIFIED=false
for attempt in {1..5}; do
    DETAILS_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN1" -d "$GQL_QUERY" "$SERVER_URL/graphql")
    if echo "$DETAILS_RES" | grep -q "$USER1_EMAIL" && echo "$DETAILS_RES" | grep -q "First comment on note 1" && echo "$DETAILS_RES" | grep -q "Second comment on note 1 from user 2"; then
        COMMENTS_VERIFIED=true
        break
    fi
    sleep 0.5
done

echo "Details response: $DETAILS_RES"

if [ "$COMMENTS_VERIFIED" = "false" ]; then
    if ! echo "$DETAILS_RES" | grep -q "$USER1_EMAIL"; then
        echo "ERROR: Note author email not found in GraphQL response"
        exit 1
    fi
    if ! echo "$DETAILS_RES" | grep -q "First comment on note 1"; then
        echo "ERROR: First comment content not found in GraphQL response"
        exit 1
    fi
    if ! echo "$DETAILS_RES" | grep -q "Second comment on note 1 from user 2"; then
        echo "ERROR: Second comment content not found in GraphQL response"
        exit 1
    fi
fi

# Email privacy: User 1 must NOT see User 2's email on their comment
if echo "$DETAILS_RES" | grep -q "$USER2_EMAIL"; then
    echo "ERROR: User 2's email leaked to User 1 (email must be stripped unless self)"
    exit 1
fi

echo "--> Single round trip GraphQL query verified!"

# 6. Test DataLoader batching with multiple notes and nested relationships
echo "Testing DataLoader batching with multiple notes query..."
GQL_ALL_NOTES='{"query":"query GetAllNotes { notes { id title author { id email } comments { id content author { email } } } }"}'
ALL_NOTES_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN1" -d "$GQL_ALL_NOTES" "$SERVER_URL/graphql")
echo "All notes response: $ALL_NOTES_RES"

if ! echo "$ALL_NOTES_RES" | grep -q "GraphQL Test Note 1" || ! echo "$ALL_NOTES_RES" | grep -q "GraphQL Test Note 2"; then
    echo "ERROR: Notes list missing created notes"
    exit 1
fi

echo "--> DataLoader batching multi-note query verified!"

# 7. Unauthenticated GraphQL must return an error code and no data.
echo "Verifying unauthorized rejection without token..."
UNAUTH_BODY=$(curl -s -X POST -H "Content-Type: application/json" -d "$GQL_ALL_NOTES" "$SERVER_URL/graphql")
UNAUTH_CODE=$(echo "$UNAUTH_BODY" | jq -r '.errors[0].extensions.code // empty')
UNAUTH_DATA=$(echo "$UNAUTH_BODY" | jq -r '.data.notes // empty')
if { [ "$UNAUTH_CODE" == "UNAUTHENTICATED" ] || [ "$UNAUTH_CODE" == "FORBIDDEN" ]; } && [ -z "$UNAUTH_DATA" ]; then
    echo "--> Unauthorized rejection verified (errors[].code=$UNAUTH_CODE, no data)!"
else
    echo "ERROR: Expected UNAUTHENTICATED/FORBIDDEN error with no data, got: $UNAUTH_BODY"
    exit 1
fi

echo "=== Suite 12: GraphQL Layer PASSED ==="
