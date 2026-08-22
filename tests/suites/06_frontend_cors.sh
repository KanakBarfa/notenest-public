#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "=== Running Frontend & CORS Preflight Tests ==="

# Check Nginx serving frontend (published on its own host port; HAProxy:80
# routes everything to Kong by design, so the SPA is not reachable there).
FRONTEND_URL="${FRONTEND_URL:-http://localhost:8082}"
frontend_status=$(curl -s -w "%{http_code}" -o /dev/null "$FRONTEND_URL/")
if [ "$frontend_status" -ne 200 ]; then
    echo "Assertion FAILED: Expected frontend status 200, got $frontend_status"
    exit 1
fi
echo "Frontend index page served successfully (status 200)."

# Preflight OPTIONS request (CORS verification)
cors_res=$(curl -s -i -X OPTIONS -H "Origin: $FRONTEND_URL" -H "Access-Control-Request-Method: GET" -H "Access-Control-Request-Headers: Authorization" "$SERVER_URL/notes")
if ! echo "$cors_res" | grep -Eiq "Access-Control-Allow-Origin: (\*|$FRONTEND_URL)"; then
    echo "Assertion FAILED: Missing/wrong Access-Control-Allow-Origin (expected * or $FRONTEND_URL)"
    echo "$cors_res"
    exit 1
fi
if ! echo "$cors_res" | grep -iq "Access-Control-Allow-Methods:.*GET"; then
    echo "Assertion FAILED: Missing Access-Control-Allow-Methods containing GET"
    echo "$cors_res"
    exit 1
fi
echo "CORS preflight request (OPTIONS) succeeded with correct headers."

echo "Frontend & CORS preflight tests passed!"
