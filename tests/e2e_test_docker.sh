#!/bin/bash
set -eo pipefail

# Docker E2E test runner wrapper
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export SERVER_URL=${SERVER_URL:-"http://localhost/api"}

echo "Waiting for backend services to be ready at $SERVER_URL..."
MAX_RETRIES=30
RETRY_COUNT=0
until curl -s -f "$SERVER_URL/notes" >/dev/null 2>&1 || [ $RETRY_COUNT -eq $MAX_RETRIES ]; do
    echo "Waiting for services ($RETRY_COUNT/$MAX_RETRIES)..."
    sleep 3
    RETRY_COUNT=$((RETRY_COUNT + 1))
done

if [ $RETRY_COUNT -eq $MAX_RETRIES ]; then
    echo "WARNING: Backend services did not respond within wait period, executing runner..."
fi

echo "Running E2E tests against Docker/Nginx setup at $SERVER_URL..."
exec "$SCRIPT_DIR/run_tests.sh" "$@"
