#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "[Phase 24] Testing Final Polish, Chaos Engineering & Load Testing Suite (v0.24)..."

PROJECT_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)

echo "1. Verifying k6 Load Test Scripts..."
if [ ! -f "$PROJECT_ROOT/tests/k6/load_test.js" ]; then
    echo "FAILED: tests/k6/load_test.js missing"
    exit 1
fi

if [ ! -f "$PROJECT_ROOT/tests/k6/chaos_scenarios.js" ]; then
    echo "FAILED: tests/k6/chaos_scenarios.js missing"
    exit 1
fi

grep -q "http_req_duration" "$PROJECT_ROOT/tests/k6/load_test.js" || { echo "load_test.js missing duration threshold"; exit 1; }
grep -q "health check ok" "$PROJECT_ROOT/tests/k6/chaos_scenarios.js" || { echo "chaos_scenarios.js missing health check validation"; exit 1; }
echo "  - k6 load test and chaos scenario scripts verified successfully"

echo "2. Verifying Chaos Experiments Automation Script..."
CHAOS_SCRIPT="$PROJECT_ROOT/scripts/chaos_experiments.sh"
if [ ! -f "$CHAOS_SCRIPT" ]; then
    echo "FAILED: scripts/chaos_experiments.sh missing"
    exit 1
fi

if [ ! -x "$CHAOS_SCRIPT" ]; then
    echo "FAILED: scripts/chaos_experiments.sh is not executable"
    exit 1
fi

echo "3b. Running real container chaos: restart app and verify recovery..."
APP_HEALTH_BEFORE=$(curl -s -o /dev/null -w "%{http_code}" "$PROJECT_ROOT/../../health" -s "http://localhost/health")
docker restart notenest-app-container >/dev/null
APP_RECOVERED=false
for attempt in {1..30}; do
    APP_HEALTH=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost/health" || echo 000)
    if [ "$APP_HEALTH" -eq 200 ]; then APP_RECOVERED=true; break; fi
    sleep 2
done
if $APP_RECOVERED; then
    echo "  - App container restarted and health recovered (HTTP 200)"
else
    echo "FAILED: App container did not recover after container-kill experiment"
    exit 1
fi

echo "3c. Verifying backing services are reachable..."
for svc_container in notenest-redis-container notenest-kafka-container; do
    if ! docker ps --format '{{.Names}}' | grep -q "^${svc_container}$"; then
        echo "FAILED: Required container $svc_container is not running"
        exit 1
    fi
done
docker exec notenest-redis-container redis-cli PING | grep -q PONG || { echo "FAILED: Redis PING failed"; exit 1; }
docker exec notenest-kafka-container kafka-broker-api-versions --bootstrap-server localhost:9092 >/dev/null 2>&1     || { echo "FAILED: Kafka broker API check failed"; exit 1; }
echo "  - Redis and Kafka verified responsive after chaos run"

echo "3. Verifying Production Operational Runbooks..."
RUNBOOK_DIR="$PROJECT_ROOT/docs/runbooks"
REQUIRED_RUNBOOKS=(
    "RUNBOOK-001.md"
    "RUNBOOK-002.md"
    "RUNBOOK-003.md"
)

for runbook in "${REQUIRED_RUNBOOKS[@]}"; do
    if [ ! -f "$RUNBOOK_DIR/$runbook" ]; then
        echo "FAILED: Runbook file missing: $runbook"
        exit 1
    fi
    grep -q "## Symptoms & Alerts" "$RUNBOOK_DIR/$runbook" || { echo "Runbook $runbook missing Symptoms section"; exit 1; }
    grep -q "## Mitigation Steps" "$RUNBOOK_DIR/$runbook" || { echo "Runbook $runbook missing Mitigation section"; exit 1; }
done
echo "  - All 3 production operational runbooks verified successfully"

echo "5. Verifying Capacity Model Documentation..."
CAPACITY_FILE="$PROJECT_ROOT/docs/capacity-model.md"
if [ ! -f "$CAPACITY_FILE" ]; then
    echo "FAILED: docs/capacity-model.md missing"
    exit 1
fi
grep -q "Resource Sizing Metrics" "$CAPACITY_FILE" || { echo "Capacity model missing Resource Sizing Metrics"; exit 1; }
grep -q "Scaling Formulae" "$CAPACITY_FILE" || { echo "Capacity model missing Scaling Formulae"; exit 1; }
echo "  - Capacity model documentation verified successfully"

echo "Phase 24 Chaos Engineering & Load Testing Suite E2E validation completed successfully!"
