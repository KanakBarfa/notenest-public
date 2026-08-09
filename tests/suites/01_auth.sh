#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Running User Authentication Tests ==="

user_a_signup='{"email":"usera@example.com","password":"password123"}'
user_a_res=$(curl -s -X POST -d "$user_a_signup" -H "Content-Type: application/json" "$SERVER_URL/signup")
user_a_id=$(echo "$user_a_res" | jq -r '.user_id')
if [ -z "$user_a_id" ] || [ "$user_a_id" == "null" ]; then
    echo "Failed to signup User A: $user_a_res"
    exit 1
fi
echo "Signed up User A with ID: $user_a_id"

assert_request "POST" "/signup" 400 "$user_a_signup"

user_b_signup='{"email":"userb@example.com","password":"password456"}'
user_b_res=$(curl -s -X POST -d "$user_b_signup" -H "Content-Type: application/json" "$SERVER_URL/signup")
user_b_id=$(echo "$user_b_res" | jq -r '.user_id')
if [ -z "$user_b_id" ] || [ "$user_b_id" == "null" ]; then
    echo "Failed to signup User B: $user_b_res"
    exit 1
fi
echo "Signed up User B with ID: $user_b_id"

user_a_login='{"email":"usera@example.com","password":"password123"}'
user_a_login_res=$(curl -s -X POST -d "$user_a_login" -H "Content-Type: application/json" "$SERVER_URL/login")
token_a=$(echo "$user_a_login_res" | jq -r '.token')
if [ -z "$token_a" ] || [ "$token_a" == "null" ]; then
    echo "Failed to login User A: $user_a_login_res"
    exit 1
fi
echo "Logged in User A, token acquired."

user_b_login='{"email":"userb@example.com","password":"password456"}'
user_b_login_res=$(curl -s -X POST -d "$user_b_login" -H "Content-Type: application/json" "$SERVER_URL/login")
token_b=$(echo "$user_b_login_res" | jq -r '.token')
if [ -z "$token_b" ] || [ "$token_b" == "null" ]; then
    echo "Failed to login User B: $user_b_login_res"
    exit 1
fi
echo "Logged in User B, token acquired."

invalid_login='{"email":"usera@example.com","password":"wrongpassword"}'
assert_request "POST" "/login" 401 "$invalid_login"

echo "Authentication tests passed!"
