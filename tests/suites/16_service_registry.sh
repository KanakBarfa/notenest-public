#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

CONSUL_HOST="localhost:8500"

echo "[Phase 16] Testing Service Registry & Discovery (Consul)..."

echo "1. Checking Consul leader status..."
LEADER=$(curl -s "http://$CONSUL_HOST/v1/status/leader")
echo "Consul Leader: $LEADER"
if [ -z "$LEADER" ] || [ "$LEADER" == '""' ]; then
    echo "FAILED: Consul agent leader not found or unreachable at $CONSUL_HOST"
    exit 1
fi

echo "2. Verifying all microservices are registered in Consul Catalog..."
CATALOG=$(curl -s "http://$CONSUL_HOST/v1/catalog/services")
echo "Catalog: $CATALOG"

REQUIRED_SERVICES=("auth-service" "user-service" "note-service" "attachment-service" "notification-service" "graphql-service")

for svc in "${REQUIRED_SERVICES[@]}"; do
    if ! echo "$CATALOG" | grep -q "\"$svc\""; then
        echo "FAILED: Service '$svc' missing in Consul service catalog"
        exit 1
    fi
    echo "  - Service '$svc': Registered OK"
done

echo "3. Verifying TTL Health Checks for active services..."
for svc in "${REQUIRED_SERVICES[@]}"; do
    PASSING=false
    for attempt in {1..5}; do
        HEALTH_RESP=$(curl -s "http://$CONSUL_HOST/v1/health/service/$svc?passing=true")
        if [ "$HEALTH_RESP" != "[]" ] && echo "$HEALTH_RESP" | grep -q '"Status": "passing"'; then
            PASSING=true
            break
        fi
        sleep 1
    done
    if [ "$PASSING" = "false" ]; then
        echo "FAILED: Service '$svc' has no passing instances in Consul"
        exit 1
    fi
    echo "  - Service '$svc': Health check passing"
done

echo "4. Testing Dynamic Service Discovery & Endpoints Resolution..."
NOTE_SVC_HEALTH=$(curl -s "http://$CONSUL_HOST/v1/health/service/note-service?passing=true")
NOTE_IP=$(echo "$NOTE_SVC_HEALTH" | grep -o '"Address": "[^"]*' | head -n 1 | cut -d'"' -f4)
NOTE_PORT=$(echo "$NOTE_SVC_HEALTH" | grep -o '"Port": [0-9]*' | head -n 1 | awk '{print $2}')
echo "Discovered Note Service instance at $NOTE_IP:$NOTE_PORT"

if [ -z "$NOTE_IP" ] || [ -z "$NOTE_PORT" ]; then
    echo "FAILED: Failed to parse Note Service address/port from Consul health response"
    exit 1
fi

echo "5. Verifying gRPC endpoints & client resolution through Nginx gateway..."
USER_EMAIL="consul_test_$RANDOM@example.com"
USER_PASS="Password123!"

AUTH_RESP=$(curl -s -X POST "$SERVER_URL/signup" \
    -H "Content-Type: application/json" \
    -d "{\"email\": \"$USER_EMAIL\", \"password\": \"$USER_PASS\"}")

USER_ID=$(echo "$AUTH_RESP" | jq -r '.user_id // empty')
if [ -z "$USER_ID" ] || [ "$USER_ID" == "null" ]; then
    echo "FAILED: Signup via gateway failed: $AUTH_RESP"
    exit 1
fi

LOGIN_RESP=$(curl -s -X POST "$SERVER_URL/login" \
    -H "Content-Type: application/json" \
    -d "{\"email\": \"$USER_EMAIL\", \"password\": \"$USER_PASS\"}")

TOKEN=$(echo "$LOGIN_RESP" | jq -r '.token // empty')
if [ -z "$TOKEN" ] || [ "$TOKEN" == "null" ]; then
    echo "FAILED: Login via gateway failed: $LOGIN_RESP"
    exit 1
fi
echo "User registered with ID $USER_ID and Token: ${TOKEN:0:15}..."

CREATE_NOTE_RESP=$(curl -s -X POST "$SERVER_URL/notes" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"title\": \"Consul Note\", \"content\": \"Created via Consul service discovery\"}")

NOTE_ID=$(echo "$CREATE_NOTE_RESP" | jq -r '.id // empty')
if [ -z "$NOTE_ID" ] || [ "$NOTE_ID" == "null" ]; then
    echo "FAILED: Create note using Consul-discovered Note Service failed: $CREATE_NOTE_RESP"
    exit 1
fi
echo "Successfully created note ID: $NOTE_ID"

echo "6. Testing Scaling & Health Deregistration in Consul..."
# Manually register a temporary dummy instance to test scaling and deregistration
DUMMY_ID="note-service-dummy-test"
curl -s -X PUT "http://$CONSUL_HOST/v1/agent/service/register" \
    -H "Content-Type: application/json" \
    -d "{\"ID\": \"$DUMMY_ID\", \"Name\": \"note-service\", \"Address\": \"172.20.0.99\", \"Port\": 50053, \"Check\": {\"CheckID\": \"service:$DUMMY_ID\", \"TTL\": \"5s\"}}"

# Send heartbeat for dummy instance
curl -s -X PUT "http://$CONSUL_HOST/v1/agent/check/pass/service:$DUMMY_ID"

INSTANCES_BEFORE=$(curl -s "http://$CONSUL_HOST/v1/health/service/note-service?passing=true" | grep -c '"ServiceID"')
echo "Active Note Service instances before deregistration: $INSTANCES_BEFORE"

# Deregister dummy instance
curl -s -X PUT "http://$CONSUL_HOST/v1/agent/service/deregister/$DUMMY_ID"
sleep 1

INSTANCES_AFTER=$(curl -s "http://$CONSUL_HOST/v1/health/service/note-service?passing=true" | grep -c '"ServiceID"')
echo "Active Note Service instances after deregistration: $INSTANCES_AFTER"

if [ "$INSTANCES_AFTER" -ge "$INSTANCES_BEFORE" ]; then
    echo "FAILED: Instance deregistration failed to decrease passing instances count"
    exit 1
fi

echo "[Phase 16] Service Registry & Discovery integration tests passed successfully!"
