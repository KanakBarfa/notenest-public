#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Running API Gateway & Rate Limiting Tests ==="

# Check if running against Gateway / Nginx
if [[ "$SERVER_URL" == *"8080"* ]]; then
    echo "Running in local standalone mode without Gateway container. Skipping Gateway assertions."
    exit 0
fi

# Test 1: Auth Offloading at Gateway
echo "Testing Auth Offloading (unauthorized requests blocked at Gateway)..."
unauth_res=$(curl -s -w "%{http_code}" -o /dev/null "$SERVER_URL/notes")
if [ "$unauth_res" -ne 401 ]; then
    echo "Failed Auth Offloading test: expected 401, got $unauth_res"
    exit 1
fi
echo "Auth Offloading verified: Unauthorized request returned 401."

# Test 2: Rate Limiting & Header Verification
echo "Testing Rate Limiting & Authenticated routing via API Gateway..."
user_signup='{"email":"gateway_test@example.com","password":"password123"}'
curl -s -X POST -d "$user_signup" -H "Content-Type: application/json" "$SERVER_URL/signup" >/dev/null || true

user_login='{"email":"gateway_test@example.com","password":"password123"}'
login_res=$(curl -s -X POST -d "$user_login" -H "Content-Type: application/json" "$SERVER_URL/login")
token=$(echo "$login_res" | jq -r '.token')

if [ -n "$token" ] && [ "$token" != "null" ]; then
    notes_code=$(curl -s -o /dev/null -w "%{http_code}" -H "Authorization: Bearer $token" "$SERVER_URL/notes")
    if [ "$notes_code" -eq 200 ]; then
        echo "Authenticated user request routed successfully through API Gateway (200 OK)."
    else
        echo "Authenticated request failed with status: $notes_code"
        exit 1
    fi
fi

# Test unauthenticated rapid request burst for 429
got_429=false
for i in {1..70}; do
    code=$(curl -s -o /dev/null -w "%{http_code}" -X POST -d '{"email":"invalid@test.com","password":"wrong"}' -H "Content-Type: application/json" "$SERVER_URL/login")
    if [ "$code" -eq 429 ]; then
        got_429=true
        break
    fi
done

if $got_429; then
    echo "Rate limiting verified: 429 Too Many Requests returned during rapid burst."
else
    echo "FAILED: Burst of 70 requests did not trigger a single 429 from gateway rate limiting"
    exit 1
fi

# Clean up rate limiting keys from Redis after test burst
cleanup_db

echo "API Gateway & Rate Limiting tests passed!"
