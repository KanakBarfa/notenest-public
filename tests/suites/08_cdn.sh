#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Running CDN Edge & Caching Integration Tests ==="

# Signup & login test user
curl -s -X POST -d '{"email":"user_cdn@example.com","password":"password123"}' -H "Content-Type: application/json" "$SERVER_URL/signup" >/dev/null || true
token_cdn=$(curl -s -X POST -d '{"email":"user_cdn@example.com","password":"password123"}' -H "Content-Type: application/json" "$SERVER_URL/login" | jq -r '.token')

if [ -z "$token_cdn" ] || [ "$token_cdn" == "null" ]; then
    echo "Failed to acquire token for CDN test."
    exit 1
fi

# Create test note
note_cdn_res=$(curl -s -X POST -d '{"title":"CDN Note","content":"Testing CDN Edge caching"}' -H "Content-Type: application/json" -H "Authorization: Bearer $token_cdn" "$SERVER_URL/notes")
note_cdn_id=$(echo "$note_cdn_res" | jq -r '.id')
if [ -z "$note_cdn_id" ] || [ "$note_cdn_id" == "null" ]; then
    echo "Failed to create note for CDN test."
    exit 1
fi

# Initialize attachment upload
filename="cdn_test_image.png"
filecontent="DUMMY_IMAGE_DATA_FOR_CDN_CACHE_VERIFICATION"
attachment_init_res=$(curl -s -X POST -d "{\"filename\":\"$filename\"}" -H "Content-Type: application/json" -H "Authorization: Bearer $token_cdn" "$SERVER_URL/notes/$note_cdn_id/attachments")
att_id=$(echo "$attachment_init_res" | jq -r '.attachment_id')
att_key=$(echo "$attachment_init_res" | jq -r '.key')
att_put_url=$(echo "$attachment_init_res" | jq -r '.url')

if [ -z "$att_id" ] || [ "$att_id" == "null" ] || [ -z "$att_put_url" ] || [ "$att_put_url" == "null" ]; then
    echo "Failed to initialize attachment for CDN test: $attachment_init_res"
    exit 1
fi

# Upload attachment via presigned PUT URL
upload_status=$(curl -s -o /dev/null -w "%{http_code}" -X PUT --data-binary "$filecontent" "$att_put_url")
if [ "$upload_status" -ne 200 ]; then
    echo "Failed to upload file to storage for CDN test. Status: $upload_status"
    exit 1
fi

# Complete attachment
filesize=${#filecontent}
complete_data="{\"attachment_id\":\"$att_id\",\"key\":\"$att_key\",\"filename\":\"$filename\",\"size\":$filesize}"
complete_status=$(curl -s -o /dev/null -w "%{http_code}" -X POST -d "$complete_data" -H "Content-Type: application/json" -H "Authorization: Bearer $token_cdn" "$SERVER_URL/notes/$note_cdn_id/attachments/complete")

if [ "$complete_status" -ne 201 ]; then
    echo "Failed to complete attachment for CDN test. Status: $complete_status"
    exit 1
fi

# Retrieve note details to obtain presigned GET URL
get_note_res=$(curl -s -H "Authorization: Bearer $token_cdn" "$SERVER_URL/notes/$note_cdn_id")
get_att_url=$(echo "$get_note_res" | jq -r '.attachments[0].url')

if [ -z "$get_att_url" ] || [ "$get_att_url" == "null" ]; then
    echo "Failed to retrieve attachment URL for CDN test."
    exit 1
fi

# Rewrite MinIO URL to go through CDN edge proxy
cdn_url=$(echo "$get_att_url" | sed 's|http://localhost:9000|http://localhost|')
get_att_url="$cdn_url"

echo "Testing CDN requests against URL: $get_att_url"

# Request 1: Expect Cache Miss (X-Cache-Status: MISS)
headers_1=$(curl -s -i "$get_att_url")
cache_status_1=$(echo "$headers_1" | grep -i "^x-cache-status:" | awk '{print $2}' | tr -d '\r\n')
cache_control_1=$(echo "$headers_1" | grep -i "^cache-control:" | tr -d '\r\n')
body_1=$(echo "$headers_1" | sed '1,/^\r\{0,1\}$/d')

echo "1st Request X-Cache-Status: '$cache_status_1'"
if [ "$cache_status_1" != "MISS" ] && [ "$cache_status_1" != "EXPIRED" ]; then
    echo "Assertion FAILED: Expected 1st request X-Cache-Status to be MISS, got '$cache_status_1'"
    exit 1
fi

if [[ "$cache_control_1" != *"public"* || "$cache_control_1" != *"max-age"* ]]; then
    echo "Assertion FAILED: Missing or invalid Cache-Control header. Got: '$cache_control_1'"
    exit 1
fi

if [ "$body_1" != "$filecontent" ]; then
    echo "Assertion FAILED: 1st request body content mismatch!"
    exit 1
fi
echo "1st Request verified: Cache MISS with valid Cache-Control header."

# Request 2: Expect Cache Hit (X-Cache-Status: HIT)
headers_2=$(curl -s -i "$get_att_url")
cache_status_2=$(echo "$headers_2" | grep -i "^x-cache-status:" | awk '{print $2}' | tr -d '\r\n')
body_2=$(echo "$headers_2" | sed '1,/^\r\{0,1\}$/d')

echo "2nd Request X-Cache-Status: '$cache_status_2'"
if [ "$cache_status_2" != "HIT" ]; then
    echo "Assertion FAILED: Expected 2nd request X-Cache-Status to be HIT, got '$cache_status_2'"
    exit 1
fi

if [ "$body_2" != "$filecontent" ]; then
    echo "Assertion FAILED: 2nd request body content mismatch on Cache HIT!"
    exit 1
fi
echo "2nd Request verified: Cache HIT with correct cached content."

# Request 3: Cache Bypass test using X-Purge-Cache header
headers_3=$(curl -s -i -H "X-Purge-Cache: 1" "$get_att_url")
cache_status_3=$(echo "$headers_3" | grep -i "^x-cache-status:" | awk '{print $2}' | tr -d '\r\n')
echo "3rd Request (Bypass) X-Cache-Status: '$cache_status_3'"
if [ "$cache_status_3" != "BYPASS" ] && [ "$cache_status_3" != "MISS" ]; then
    echo "Assertion FAILED: Expected cache bypass status, got '$cache_status_3'"
    exit 1
fi
echo "3rd Request verified: Cache BYPASS / purge header handled correctly."

# Cleanup note and attachment
curl -s -X DELETE -H "Authorization: Bearer $token_cdn" "$SERVER_URL/notes/$note_cdn_id" >/dev/null

echo "CDN Edge & Caching tests passed successfully!"
