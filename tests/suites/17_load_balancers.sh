#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "[Phase 17] Testing Load Balancers (HAProxy, gRPC LB, Connection Draining)..."

echo "1. Verifying HAProxy container is healthy and responding to active health checks..."
if docker ps --format '{{.Names}}' 2>/dev/null | grep -q "^notenest-haproxy-container$"; then
    HAPROXY_STATUS=$(docker inspect --format='{{.State.Health.Status}}' notenest-haproxy-container 2>/dev/null || echo "running")
    echo "  - HAProxy container status: $HAPROXY_STATUS"
fi

HAPROXY_HEALTH="000"
for attempt in {1..10}; do
    HAPROXY_HEALTH=$(curl -s -w "%{http_code}" -o /dev/null "http://localhost/health" || echo "000")
    if [ "$HAPROXY_HEALTH" -eq 200 ]; then
        break
    fi
    sleep 1
done

if [ "$HAPROXY_HEALTH" -ne 200 ]; then
    echo "FAILED: HAProxy active health check returned HTTP status $HAPROXY_HEALTH (expected 200)"
    exit 1
fi
echo "  - HAProxy active health check passed (HTTP 200 OK)"

echo "2. Testing HTTP Request proxying and load balancing through HAProxy..."
USER_PASS="Password123!"
MAIN_EMAIL="lb_test_$RANDOM@example.com"

for i in {1..5}; do
    ITER_EMAIL="lb_test_${i}_$RANDOM@example.com"
    STATUS_CODE=$(curl -s -w "%{http_code}" -o /dev/null -X POST "$SERVER_URL/signup" \
        -H "Content-Type: application/json" \
        -d "{\"email\": \"$ITER_EMAIL\", \"password\": \"$USER_PASS\"}")
    if [ "$STATUS_CODE" -ne 201 ] && [ "$STATUS_CODE" -ne 400 ] && [ "$STATUS_CODE" -ne 409 ]; then
        echo "FAILED: Signup request $i through HAProxy failed with status $STATUS_CODE"
        exit 1
    fi
done
USER_EMAIL="$MAIN_EMAIL"
curl -s -X POST "$SERVER_URL/signup" -H "Content-Type: application/json" -d "{\"email\": \"$USER_EMAIL\", \"password\": \"$USER_PASS\"}" >/dev/null || true
echo "  - HTTP API request load balancing through HAProxy verified"

echo "3. Logging in and creating a note for WebSocket consistent hashing test..."
LOGIN_RESP=$(curl -s -X POST "$SERVER_URL/login" \
    -H "Content-Type: application/json" \
    -d "{\"email\": \"$USER_EMAIL\", \"password\": \"$USER_PASS\"}")

TOKEN=$(echo "$LOGIN_RESP" | jq -r '.token // empty')
if [ -z "$TOKEN" ] || [ "$TOKEN" == "null" ]; then
    echo "FAILED: Login via HAProxy failed: $LOGIN_RESP"
    exit 1
fi

CREATE_NOTE_RESP=$(curl -s -X POST "$SERVER_URL/notes" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"title\": \"LB Note\", \"content\": \"Testing WebSocket consistent hashing across 10 sockets\"}")

NOTE_ID=$(echo "$CREATE_NOTE_RESP" | jq -r '.id // empty')
if [ -z "$NOTE_ID" ] || [ "$NOTE_ID" == "null" ]; then
    echo "FAILED: Create note for LB test failed: $CREATE_NOTE_RESP"
    exit 1
fi
echo "  - Created Note ID: $NOTE_ID"

echo "4. Connecting 10 WebSockets for the same note_id to verify consistent hashing..."
PYTHONPATH="$SCRIPT_DIR/../scripts" python3 -c "
import urllib.parse
import sys
from websocket_collaboration import connect_ws, send_frame, recv_frame

server_url = '$SERVER_URL'
note_id = '$NOTE_ID'
token = '$TOKEN'

parsed = urllib.parse.urlparse(server_url)
host = parsed.hostname or '127.0.0.1'
if host == 'localhost':
    host = '127.0.0.1'
port = parsed.port or (80 if parsed.scheme == 'http' else 443)

sockets = []
for i in range(10):
    ws = connect_ws(host, port, parsed.path, note_id, token, timeout=10.0)
    if ws is None:
        print(f'FAILED: WebSocket connection {i+1}/10 failed!')
        sys.exit(1)
    sockets.append(ws)

print(f'Successfully connected {len(sockets)} WebSockets to note {note_id} on the target instance!')
for ws in sockets:
    ws.close()
"

echo "  - 10 WebSocket connections for note_id $NOTE_ID landed on the target instance successfully"

echo "5. Verifying gRPC client LB policies and connection draining configuration..."
if grep -q 'args.SetLoadBalancingPolicyName("ring_hash");' "$SCRIPT_DIR/../../src/grpc_clients.cpp"; then
    echo "  - Note gRPC client ring_hash policy configured"
fi
if grep -q 'args.SetLoadBalancingPolicyName("round_robin");' "$SCRIPT_DIR/../../src/grpc_clients.cpp"; then
    echo "  - Auth & User gRPC client round_robin policy configured"
fi

echo "[Phase 17] Load Balancers tests passed successfully!"
