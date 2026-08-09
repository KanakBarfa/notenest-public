#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Running Attachment & S3 Integration Tests ==="

curl -s -X POST -d '{"email":"user_att@example.com","password":"password123"}' -H "Content-Type: application/json" "$SERVER_URL/signup" >/dev/null || true
token_att=$(curl -s -X POST -d '{"email":"user_att@example.com","password":"password123"}' -H "Content-Type: application/json" "$SERVER_URL/login" | jq -r '.token')

if [ -z "$token_att" ] || [ "$token_att" == "null" ]; then
    echo "Failed to acquire token for Attachment test."
    exit 1
fi

note_att_data='{"title":"Note for attachment","content":"This note will have attachments"}'
note_att_res=$(curl -s -X POST -d "$note_att_data" -H "Content-Type: application/json" -H "Authorization: Bearer $token_att" "$SERVER_URL/notes")
note_att_id=$(echo "$note_att_res" | jq -r '.id')
if [ -z "$note_att_id" ] || [ "$note_att_id" == "null" ]; then
    echo "Failed to create note for attachment: $note_att_res"
    exit 1
fi
echo "Created note for attachment: $note_att_id"

filename="testfile.txt"
filecontent="Hello MinIO! This is a test file for S3 attachments."
attachment_init_res=$(curl -s -X POST -d "{\"filename\":\"$filename\"}" -H "Content-Type: application/json" -H "Authorization: Bearer $token_att" "$SERVER_URL/notes/$note_att_id/attachments")
att_id=$(echo "$attachment_init_res" | jq -r '.attachment_id')
att_key=$(echo "$attachment_init_res" | jq -r '.key')
att_put_url=$(echo "$attachment_init_res" | jq -r '.url')

if [ -z "$att_id" ] || [ "$att_id" == "null" ] || [ -z "$att_put_url" ] || [ "$att_put_url" == "null" ]; then
    echo "Failed to initialize attachment upload: $attachment_init_res"
    exit 1
fi
echo "Initialized attachment. ID: $att_id, Key: $att_key"

upload_status=$(curl -s -o /dev/null -w "%{http_code}" -X PUT --data-binary "$filecontent" "$att_put_url")
if [ "$upload_status" -ne 200 ]; then
    echo "Failed to upload file to S3. Status: $upload_status"
    exit 1
fi
echo "Uploaded file directly to S3 (status 200)."

filesize=${#filecontent}
complete_data="{\"attachment_id\":\"$att_id\",\"key\":\"$att_key\",\"filename\":\"$filename\",\"size\":$filesize}"
complete_status=$(curl -s -o /dev/null -w "%{http_code}" -X POST -d "$complete_data" -H "Content-Type: application/json" -H "Authorization: Bearer $token_att" "$SERVER_URL/notes/$note_att_id/attachments/complete")

if [ "$complete_status" -ne 201 ]; then
    echo "Failed to complete attachment. Status: $complete_status"
    exit 1
fi
echo "Completed attachment upload (status 201)."

# Test: 100MB per-user attachment quota check
exceeding_complete_data="{\"attachment_id\":\"quota_test_id\",\"key\":\"quota_test_key\",\"filename\":\"huge.bin\",\"size\":104857601}"
assert_request "POST" "/notes/$note_att_id/attachments/complete" 400 "$exceeding_complete_data" "$token_att"
echo "Verified 100MB user attachment storage quota check (returned status 400)."

get_note_res=$(curl -s -H "Authorization: Bearer $token_att" "$SERVER_URL/notes/$note_att_id")
get_att_id=$(echo "$get_note_res" | jq -r '.attachments[0].id')
get_att_url=$(echo "$get_note_res" | jq -r '.attachments[0].url')

if [ "$get_att_id" != "$att_id" ] || [ -z "$get_att_url" ] || [ "$get_att_url" == "null" ]; then
    echo "Attachment not correctly retrieved with note: $get_note_res"
    exit 1
fi
echo "Verified attachment inside note. GET URL: $get_att_url"

downloaded_content=$(curl -s "$get_att_url")
if [ "$downloaded_content" != "$filecontent" ]; then
    echo "Downloaded content mismatch! Got: '$downloaded_content', Expected: '$filecontent'"
    exit 1
fi
echo "Successfully downloaded and verified attachment content directly from storage."

# Test: Delete individual attachment directly
assert_request "DELETE" "/notes/$note_att_id/attachments/$att_id" 204 "" "$token_att"
echo "Deleted individual attachment: $att_id"

# Verify note no longer contains the attachment
get_note_after_att_del=$(curl -s -H "Authorization: Bearer $token_att" "$SERVER_URL/notes/$note_att_id")
att_count=$(echo "$get_note_after_att_del" | jq '.attachments | length')
if [ "$att_count" -ne 0 ]; then
    echo "Assertion FAILED: Expected 0 attachments after delete, got $att_count"
    exit 1
fi

assert_request "DELETE" "/notes/$note_att_id" 204 "" "$token_att"
echo "Deleted note containing attachment."

verify_deleted_status=$(curl -s -H "X-Purge-Cache: 1" -o /dev/null -w "%{http_code}" "$get_att_url")
if [ "$verify_deleted_status" -eq 200 ]; then
    echo "Assertion FAILED: Object was not deleted from S3!"
    exit 1
fi
echo "Verified object deleted from S3 (GET returned status $verify_deleted_status)."

echo "Attachment & S3 integration tests passed!"
