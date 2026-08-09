#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Running Expired Token Tests ==="

curl -s -X POST -d '{"email":"user_exp@example.com","password":"password123"}' -H "Content-Type: application/json" "$SERVER_URL/signup" >/dev/null || true

expired_login='{"email":"user_exp@example.com","password":"password123","expiry_seconds":1}'
expired_res=$(curl -s -X POST -d "$expired_login" -H "Content-Type: application/json" "$SERVER_URL/login")
expired_token=$(echo "$expired_res" | jq -r '.token')
if [ -z "$expired_token" ] || [ "$expired_token" == "null" ]; then
    echo "Failed to get temporary token: $expired_res"
    exit 1
fi

assert_request "GET" "/notes" 200 "" "$expired_token"

echo "Waiting for token to expire..."
sleep 2

assert_request "GET" "/notes" 401 "" "$expired_token"

echo "Expired token tests passed!"
