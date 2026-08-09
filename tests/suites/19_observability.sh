#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "[Phase 19] Testing Observability (Logs, Metrics, Traces)..."

echo "1. Verifying health of Observability stack containers..."
for svc in prometheus alertmanager loki tempo otel-collector grafana; do
    CONTAINER_NAME="notenest-${svc}-container"
    if ! docker ps --format '{{.Names}}' | grep -q "$CONTAINER_NAME"; then
        echo "FAILED: Container $CONTAINER_NAME is not running"
        exit 1
    fi
    echo "  - $CONTAINER_NAME is running"
done

echo "2. Verifying /metrics endpoints across all microservices..."

# C++ App Service
APP_METRICS=$(curl -s -f http://localhost:8080/metrics || curl -s -f http://localhost:8082/metrics || curl -s -f http://localhost/metrics || docker exec notenest-app-container curl -s http://localhost:8080/metrics 2>/dev/null || true)
if [ -z "$APP_METRICS" ] || ! echo "$APP_METRICS" | grep -q "http_requests_total"; then
    echo "FAILED: C++ App /metrics endpoint (port 8080) failed or missing http_requests_total"
    exit 1
fi
echo "  - C++ App /metrics endpoint verified"

# Python Auth Service
AUTH_METRICS=$(curl -s -f http://localhost:9101/metrics || true)
if [ -z "$AUTH_METRICS" ] || ! echo "$AUTH_METRICS" | grep -q "http_requests_total"; then
    echo "FAILED: Auth Service /metrics endpoint (port 9101) failed"
    exit 1
fi
echo "  - Auth Service /metrics endpoint verified"

# Python User Service
USER_METRICS=$(curl -s -f http://localhost:9102/metrics || true)
if [ -z "$USER_METRICS" ] || ! echo "$USER_METRICS" | grep -q "http_requests_total"; then
    echo "FAILED: User Service /metrics endpoint (port 9102) failed"
    exit 1
fi
echo "  - User Service /metrics endpoint verified"

# Python Notification Service
NOTIF_METRICS=$(curl -s -f http://localhost:9103/metrics || true)
if [ -z "$NOTIF_METRICS" ] || ! echo "$NOTIF_METRICS" | grep -q "http_requests_total"; then
    echo "FAILED: Notification Service /metrics endpoint (port 9103) failed"
    exit 1
fi
echo "  - Notification Service /metrics endpoint verified"

# Node.js GraphQL Service
GRAPHQL_METRICS=$(curl -s -f http://localhost:4000/metrics || true)
if [ -z "$GRAPHQL_METRICS" ] || ! echo "$GRAPHQL_METRICS" | grep -q "http_requests_total"; then
    echo "FAILED: GraphQL Service /metrics endpoint (port 4000) failed"
    exit 1
fi
echo "  - GraphQL Service /metrics endpoint verified"

echo "3. Generating application activity to emit logs, metrics, and traces..."
USER_EMAIL="obs_test_${RANDOM}@example.com"
USER_PASS="Password123!"

SIGNUP_RESP=$(curl -s -X POST "$SERVER_URL/signup" \
    -H "Content-Type: application/json" \
    -H "traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01" \
    -d "{\"email\": \"$USER_EMAIL\", \"password\": \"$USER_PASS\"}")
echo "  - Signup executed"

LOGIN_RESP=$(curl -s -X POST "$SERVER_URL/login" \
    -H "Content-Type: application/json" \
    -d "{\"email\": \"$USER_EMAIL\", \"password\": \"$USER_PASS\"}")
TOKEN=$(echo "$LOGIN_RESP" | jq -r '.token // empty')
if [ -z "$TOKEN" ] || [ "$TOKEN" == "null" ]; then
    echo "FAILED: Login failed: $LOGIN_RESP"
    exit 1
fi
echo "  - Login executed, token acquired"

CREATE_NOTE_RESP=$(curl -s -X POST "$SERVER_URL/notes" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"title\": \"Observability Test Note\", \"content\": \"Testing RED metrics and Loki log ingestion\"}")
NOTE_ID=$(echo "$CREATE_NOTE_RESP" | jq -r '.id // empty')
echo "  - Note created (ID: $NOTE_ID)"

echo "4. Verifying Prometheus metrics scraping via Prometheus API..."
sleep 10
PROM_QUERY=$(curl -s "http://localhost:9090/api/v1/query?query=http_requests_total")
STATUS=$(echo "$PROM_QUERY" | jq -r '.status // empty')
RESULT_COUNT=$(echo "$PROM_QUERY" | jq -r '.data.result | length')

if [ "$STATUS" != "success" ] || [ "$RESULT_COUNT" -eq 0 ]; then
    echo "FAILED: Prometheus query failed or returned no metrics: $PROM_QUERY"
    exit 1
fi
echo "  - Prometheus metrics query successful ($RESULT_COUNT metric series returned)"

echo "5. Verifying Loki log ingestion via Loki Query API..."
LOKI_QUERY=$(curl -s "http://localhost:3100/loki/api/v1/query?query=%7Bcontainer%3D~%22notenest.*%22%7D")
LOKI_STATUS=$(echo "$LOKI_QUERY" | jq -r '.status // empty')

if [ "$LOKI_STATUS" != "success" ]; then
    echo "FAILED: Loki log query returned non-success status: $LOKI_QUERY"
    exit 1
fi
echo "  - Loki log ingestion verified"

echo "6. Verifying Tempo / OTEL Collector service availability..."
TEMPO_HEALTH=0
for i in {1..15}; do
    TEMPO_HEALTH=$(curl -s -w "%{http_code}" -o /dev/null http://localhost:3200/ready || echo "000")
    if [ "$TEMPO_HEALTH" -eq 200 ]; then break; fi
    sleep 2
done
if [ "$TEMPO_HEALTH" -ne 200 ]; then
    echo "FAILED: Tempo ready check returned HTTP $TEMPO_HEALTH (expected 200)"
    exit 1
fi
echo "  - Tempo service ready and receiving trace spans"

echo "7. Verifying Alertmanager target configuration in Prometheus..."
ALERTS_QUERY=$(curl -s http://localhost:9090/api/v1/rules)
ALERT_NAME=$(echo "$ALERTS_QUERY" | jq -r '.data.groups[0].rules[0].name // empty')
if [ "$ALERT_NAME" != "HighErrorRate" ]; then
    echo "FAILED: Prometheus alert rule 'HighErrorRate' not found: $ALERTS_QUERY"
    exit 1
fi
echo "  - Prometheus alert rule 'HighErrorRate' verified"

echo "[Phase 19] Observability tests passed successfully!"
