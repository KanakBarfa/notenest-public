#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

if command -v grpcurl &>/dev/null; then
    GRPCURL="grpcurl"
elif [ -f "${HOME}/go/bin/grpcurl" ]; then
    GRPCURL="${HOME}/go/bin/grpcurl"
else
    GRPCURL="grpcurl"
fi

echo "[Phase 14] Testing Microservices & gRPC Architecture..."

AUTH_HOST="localhost:50051"
USER_HOST="localhost:50052"
NOTE_HOST="localhost:50053"
ATTACHMENT_HOST="localhost:50054"

if [ -n "$DOCKER_MODE" ]; then
    AUTH_HOST="localhost:50051"
    USER_HOST="localhost:50052"
    NOTE_HOST="localhost:50053"
    ATTACHMENT_HOST="localhost:50054"
fi

TEST_EMAIL="grpc_phase14_$RANDOM@example.com"
TEST_PASS="Password123!"

echo "1. Testing Auth gRPC Service (Signup, Login, VerifyToken)..."
SIGNUP_RESP=$($GRPCURL -plaintext -d "{\"email\": \"$TEST_EMAIL\", \"password\": \"$TEST_PASS\"}" "$AUTH_HOST" notenest.auth.AuthService/Signup)
echo "Signup Response: $SIGNUP_RESP"

USER_ID=$(echo "$SIGNUP_RESP" | grep -o '"userId": "[^"]*' | cut -d'"' -f4 || echo "$SIGNUP_RESP" | grep -o '"user_id": "[^"]*' | cut -d'"' -f4)
TOKEN=$(echo "$SIGNUP_RESP" | grep -o '"token": "[^"]*' | cut -d'"' -f4)

if [ -z "$USER_ID" ] || [ -z "$TOKEN" ]; then
    echo "FAILED: Auth Signup failed or returned missing fields"
    exit 1
fi
echo "User ID: $USER_ID"
echo "Token: $TOKEN"

echo "Testing Auth gRPC Login..."
LOGIN_RESP=$($GRPCURL -plaintext -d "{\"email\": \"$TEST_EMAIL\", \"password\": \"$TEST_PASS\"}" "$AUTH_HOST" notenest.auth.AuthService/Login)
echo "Login Response: $LOGIN_RESP"

echo "Testing Auth gRPC VerifyToken..."
VERIFY_RESP=$($GRPCURL -plaintext -d "{\"token\": \"$TOKEN\"}" "$AUTH_HOST" notenest.auth.AuthService/VerifyToken)
echo "VerifyToken Response: $VERIFY_RESP"
if ! echo "$VERIFY_RESP" | grep -q '"valid": true'; then
    echo "FAILED: Auth VerifyToken did not return valid: true"
    exit 1
fi

echo "2. Testing User gRPC Service (GetUserProfile, GetUsersByIDs)..."
PROFILE_RESP=$($GRPCURL -plaintext -d "{\"user_id\": \"$USER_ID\"}" "$USER_HOST" notenest.user.UserService/GetUserProfile)
echo "GetUserProfile Response: $PROFILE_RESP"
if ! echo "$PROFILE_RESP" | grep -q "$TEST_EMAIL"; then
    echo "FAILED: User profile email mismatch"
    exit 1
fi

BATCH_RESP=$($GRPCURL -plaintext -d "{\"user_ids\": [\"$USER_ID\"]}" "$USER_HOST" notenest.user.UserService/GetUsersByIDs)
echo "GetUsersByIDs Response: $BATCH_RESP"

echo "3. Testing Note gRPC Service (CreateNote, ListNotes)..."
CREATE_NOTE_RESP=$($GRPCURL -plaintext -d "{\"owner_id\": \"$USER_ID\", \"title\": \"gRPC Title\", \"content\": \"gRPC Content\"}" "$NOTE_HOST" notenest.note.NoteService/CreateNote)
echo "CreateNote Response: $CREATE_NOTE_RESP"

NOTE_ID=$(echo "$CREATE_NOTE_RESP" | grep -o '"id": "[^"]*' | cut -d'"' -f4)
if [ -z "$NOTE_ID" ]; then
    echo "FAILED: Note creation via gRPC failed"
    exit 1
fi

LIST_NOTES_RESP=$($GRPCURL -plaintext -d "{\"owner_id\": \"$USER_ID\"}" "$NOTE_HOST" notenest.note.NoteService/ListNotes)
echo "ListNotes Response: $LIST_NOTES_RESP"
if ! echo "$LIST_NOTES_RESP" | grep -q "$NOTE_ID"; then
    echo "FAILED: Created note ID missing in ListNotes"
    exit 1
fi

echo "4. Testing Attachment gRPC Service (GenerateUploadUrl)..."
ATT_RESP=$($GRPCURL -plaintext -d "{\"note_id\": \"$NOTE_ID\", \"user_id\": \"$USER_ID\", \"filename\": \"doc.pdf\"}" "$ATTACHMENT_HOST" notenest.attachment.AttachmentService/GenerateUploadUrl)
echo "GenerateUploadUrl Response: $ATT_RESP"
if ! echo "$ATT_RESP" | grep -q '"uploadUrl"'; then
    if ! echo "$ATT_RESP" | grep -q '"upload_url"'; then
        echo "FAILED: Presigned upload URL generation failed"
        exit 1
    fi
fi

echo "5. Testing Fault Tolerance: Shutting down Auth Service..."
if docker compose ps | grep -q "notenest-auth-container"; then
    docker compose stop auth
    echo "Stopped Auth Service container."
    
    echo "Verifying Note REST API still works for already-authenticated user ($TOKEN)..."
    REST_NOTES=$(curl -s -H "Authorization: Bearer $TOKEN" "$SERVER_URL/notes")
    echo "REST Notes Response: $REST_NOTES"
    if ! echo "$REST_NOTES" | grep -q "$NOTE_ID"; then
        echo "FAILED: Note service failed while Auth service was offline"
        docker compose start auth
        exit 1
    fi

    echo "Restarting Auth Service..."
    docker compose start auth
    sleep 3
else
    echo "Local execution mode: Skipping docker compose stop auth"
fi

echo "[Phase 14] Microservices & gRPC integration tests passed successfully!"
