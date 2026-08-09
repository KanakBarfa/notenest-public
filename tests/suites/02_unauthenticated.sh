#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Running Unauthenticated Requests Tests ==="

assert_request "GET" "/notes" 401
assert_request "GET" "/notes" 401 "" "invalidtokenformat"

echo "Unauthenticated request tests passed!"
