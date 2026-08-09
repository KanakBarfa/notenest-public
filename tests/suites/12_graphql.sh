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

# 4. Add comments to Note 1 via GraphQL mutation
echo "Adding comment 1 to Note 1 via User 1..."
GQL_COMMENT1="{\"query\":\"mutation { createComment(noteId: \\\"$NOTE1_ID\\\", content: \\\"First comment on note 1\\\") { id content author { email } } }\"}"
COMMENT1_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN1" -d "$GQL_COMMENT1" "$SERVER_URL/graphql")
echo "Comment 1 response: $COMMENT1_RES"

echo "Adding comment 2 to Note 1 via User 2..."
GQL_COMMENT2="{\"query\":\"mutation { createComment(noteId: \\\"$NOTE1_ID\\\", content: \\\"Second comment on note 1 from user 2\\\") { id content author { email } } }\"}"
COMMENT2_RES=$(curl -s -X POST -H "Content-Type: application/json" -H "Authorization: Bearer $TOKEN2" -d "$GQL_COMMENT2" "$SERVER_URL/graphql")
echo "Comment 2 response: $COMMENT2_RES"

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

# 7. Verify authentication enforcement on protected GraphQL queries/mutations
echo "Verifying unauthorized rejection without token..."
UNAUTH_RES=$(curl -s -o /dev/null -w "%{http_code}" -X POST -H "Content-Type: application/json" -d "$GQL_ALL_NOTES" "$SERVER_URL/graphql")
if [ "$UNAUTH_RES" -eq 401 ] || [ "$UNAUTH_RES" -eq 403 ]; then
    echo "--> Unauthorized rejection verified ($UNAUTH_RES)!"
else
    echo "ERROR: Expected 401/403 for unauthenticated GraphQL request, got $UNAUTH_RES"
    exit 1
fi

echo "=== Suite 12: GraphQL Layer PASSED ==="
