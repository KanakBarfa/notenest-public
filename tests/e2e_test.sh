#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# Load environment variables from .env if present
if [ -f .env ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        if [[ ! "$line" =~ ^# ]] && [[ -n "$line" ]]; then
            export "$line"
        fi
    done < .env
fi

PORT=${PORT:-8080}
export SERVER_URL="http://localhost:$PORT"
TEMP_LOG="server.log"

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        echo "Shutting down server (PID: $SERVER_PID)..."
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -f "$TEMP_LOG"
}
trap cleanup EXIT

DB_CONN=${DATABASE_URL:-"host=localhost port=5432 dbname=postgres user=postgres password=pass"}
psql "$DB_CONN" -c "DELETE FROM users WHERE email LIKE '%@example.com' OR email LIKE 'test_%';" >/dev/null 2>&1 || true

echo "Starting NoteNest server on port $PORT..."
./notenest > "$TEMP_LOG" 2>&1 &
SERVER_PID=$!

for i in {1..20}; do
    if curl -s "$SERVER_URL/signup" >/dev/null; then
        break
    fi
    sleep 0.1
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "Server failed to start. Logs:"
        cat "$TEMP_LOG"
        exit 1
    fi
done

echo "Server started successfully."

"$SCRIPT_DIR/run_tests.sh" "$@"

echo "Local E2E test run finished successfully."
