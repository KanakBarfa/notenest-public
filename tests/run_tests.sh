#!/bin/bash
set -eo pipefail

# NoteNest Test Runner & Smart Change Detector

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SUITES_DIR="$SCRIPT_DIR/suites"
source "$SCRIPT_DIR/common.sh"

MODE="all"
SELECTED_SUITES=()

usage() {
    echo "Usage: $0 [--all|--smart|<suite_name_or_number>...]"
    echo ""
    echo "Modes:"
    echo "  --all        Run all test suites (default)"
    echo "  --smart      Detect modified files via git and run only affected suites"
    echo "  <names...>   Run specific test suites (e.g. auth, cache, 07_attachments)"
    exit 1
}

# Parse command line flags
while [[ $# -gt 0 ]]; do
    case "$1" in
        --all)
            MODE="all"
            shift
            ;;
        --smart)
            MODE="smart"
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            MODE="custom"
            SELECTED_SUITES+=("$1")
            shift
            ;;
    esac
done

detect_smart_suites() {
    local changed_files
    changed_files=$(git status --porcelain 2>/dev/null | awk '{print $2}' || true)
    changed_files+=$'\n'$(git diff --name-only HEAD 2>/dev/null || true)
    
    if [ -z "$(echo "$changed_files" | tr -d '[:space:]')" ]; then
        echo "[Smart Runner] No git changes detected. Defaulting to running ALL suites."
        MODE="all"
        return
    fi

    echo "[Smart Runner] Detected changed files:"
    echo "$changed_files" | sed 's/^/  - /' | sort -u
    echo ""

    local run_auth=false
    local run_unauth=false
    local run_isolation=false
    local run_cache=false
    local run_exp=false
    local run_frontend=false
    local run_attachments=false
    local run_cdn=false
    local run_gateway=false
    local run_sse=false
    local run_ws=false
    local run_graphql=false
    local run_pooling=false
    local run_grpc=false
    local run_kafka=false
    local run_consul=false
    local run_lb=false
    local run_network=false
    local run_obs=false
    local run_k8s=false
    local run_repl=false
    local run_tf=false
    local run_cicd=false
    local run_chaos=false
    local run_all_flag=false

    while IFS= read -r file; do
        [ -z "$file" ] && continue
        case "$file" in
            scripts/chaos*|tests/k6/*|docs/runbooks/*|docs/capacity-model.md)
                run_chaos=true
                ;;
            .github/*)
                run_cicd=true
                ;;
            deployments/terraform/*)
                run_tf=true
                ;;
            deployments/helm/*)
                run_k8s=true
                ;;
            deployments/prometheus/*|deployments/loki/*|deployments/promtail/*|deployments/tempo/*|deployments/otel-collector/*|deployments/alertmanager/*|deployments/grafana/*|src/observability.*|include/notenest/observability.hpp|services/obs_helper.py)
                run_obs=true
                ;;
            deployments/haproxy.cfg|deployments/*)
                run_lb=true
                run_frontend=true
                run_cdn=true
                run_gateway=true
                run_sse=true
                run_ws=true
                run_graphql=true
                run_pooling=true
                run_obs=true
                ;;
            services/consul_client.py|src/consul_client.*|include/notenest/consul_client.*)
                run_consul=true
                run_lb=true
                ;;
            services/notification/*|src/kafka_producer.*|include/notenest/kafka_producer.*|src/outbox_relay.*|include/notenest/outbox_relay.*|src/rabbitmq_client.*|include/notenest/rabbitmq_client.*)
                run_kafka=true
                run_obs=true
                ;;
            protos/*|services/auth/*|services/user/*|src/grpc_*|include/notenest/grpc_*)
                run_grpc=true
                run_kafka=true
                run_obs=true
                ;;
            src/auth_*|include/notenest/auth_*|src/user_*|include/notenest/user_*|include/notenest/crypto.hpp)
                run_auth=true
                run_unauth=true
                run_exp=true
                run_gateway=true
                run_graphql=true
                run_grpc=true
                run_obs=true
                ;;
            src/event_bus.*|include/notenest/event_bus.*)
                run_sse=true
                ;;
            src/room_registry.*|include/notenest/room_registry.*|include/notenest/websocket.hpp)
                run_ws=true
                ;;
            services/graphql/*)
                run_graphql=true
                run_pooling=true
                run_obs=true
                ;;
            src/db_pool.*|include/notenest/db_pool.*|include/notenest/pool.hpp|include/notenest/circuit_breaker.hpp)
                run_pooling=true
                ;;
            src/note_store.*|include/notenest/note_store.*|src/note_repo.*|include/notenest/note_repo.*|db/*)
                run_isolation=true
                run_cache=true
                run_sse=true
                run_ws=true
                run_graphql=true
                run_pooling=true
                run_grpc=true
                run_obs=true
                ;;
            src/redis_cache.*|include/notenest/redis_cache.*|include/notenest/cache.hpp)
                run_cache=true
                run_gateway=true
                run_pooling=true
                ;;
            src/object_store.*|include/notenest/object_store.*|include/notenest/attachment.hpp)
                run_attachments=true
                run_cdn=true
                run_pooling=true
                run_grpc=true
                ;;
            frontend/*|deployments/*)
                run_frontend=true
                run_cdn=true
                run_gateway=true
                run_sse=true
                run_ws=true
                run_graphql=true
                run_pooling=true
                run_obs=true
                ;;
            src/router.*|include/notenest/router.hpp)
                run_auth=true
                run_isolation=true
                run_frontend=true
                run_attachments=true
                run_cdn=true
                run_gateway=true
                run_sse=true
                run_ws=true
                run_graphql=true
                run_pooling=true
                run_grpc=true
                run_obs=true
                ;;
            src/main.cpp|Makefile|Dockerfile|docker-compose.yml|tests/*)
                run_all_flag=true
                ;;
        esac
    done <<< "$changed_files"

    if $run_all_flag; then
        echo "[Smart Runner] Core or shared files changed. Running ALL suites."
        MODE="all"
        return
    fi

    SELECTED_SUITES=()
    $run_auth && SELECTED_SUITES+=("01_auth.sh")
    $run_unauth && SELECTED_SUITES+=("02_unauthenticated.sh")
    $run_isolation && SELECTED_SUITES+=("03_note_isolation.sh")
    $run_cache && SELECTED_SUITES+=("04_cache.sh")
    $run_exp && SELECTED_SUITES+=("05_expired_token.sh")
    $run_frontend && SELECTED_SUITES+=("06_frontend_cors.sh")
    $run_attachments && SELECTED_SUITES+=("07_attachments.sh")
    $run_cdn && SELECTED_SUITES+=("08_cdn.sh")
    $run_gateway && SELECTED_SUITES+=("09_rate_limit_gateway.sh")
    $run_sse && SELECTED_SUITES+=("10_sse_notifications.sh")
    $run_ws && SELECTED_SUITES+=("11_websockets.sh")
    $run_graphql && SELECTED_SUITES+=("12_graphql.sh")
    $run_pooling && SELECTED_SUITES+=("13_connection_pooling.sh")
    $run_grpc && SELECTED_SUITES+=("14_microservices_grpc.sh")
    $run_kafka && SELECTED_SUITES+=("15_kafka_outbox.sh")
    $run_consul && SELECTED_SUITES+=("16_service_registry.sh")
    $run_lb && SELECTED_SUITES+=("17_load_balancers.sh")
    $run_network && SELECTED_SUITES+=("18_network_segmentation.sh")
    $run_obs && SELECTED_SUITES+=("19_observability.sh")
    $run_k8s && SELECTED_SUITES+=("20_kubernetes_helm.sh")
    $run_repl && SELECTED_SUITES+=("21_db_replication.sh")
    $run_tf && SELECTED_SUITES+=("22_terraform.sh")
    $run_cicd && SELECTED_SUITES+=("23_cicd_workflows.sh")
    $run_chaos && SELECTED_SUITES+=("24_chaos_load_testing.sh")

    if [ ${#SELECTED_SUITES[@]} -eq 0 ]; then
        echo "[Smart Runner] Unrecognized change pattern. Running ALL suites."
        MODE="all"
    fi
}

if [ "$MODE" == "smart" ]; then
    detect_smart_suites
fi

SUITES_TO_RUN=()
if [ "$MODE" == "all" ]; then
    for s in "$SUITES_DIR"/*.sh; do
        SUITES_TO_RUN+=("$(basename "$s")")
    done
else
    for item in "${SELECTED_SUITES[@]}"; do
        found=false
        for s in "$SUITES_DIR"/*.sh; do
            base=$(basename "$s")
            if [[ "$base" == *"$item"* ]]; then
                SUITES_TO_RUN+=("$base")
                found=true
                break
            fi
        done
        if ! $found; then
            echo "Error: Test suite matching '$item' not found."
            exit 1
        fi
    done
fi

echo "========================================="
echo "        NoteNest E2E Test Suite          "
echo "========================================="
echo "Target URL: $SERVER_URL"
echo "Mode: $MODE"
echo "Suites to execute:"
for s in "${SUITES_TO_RUN[@]}"; do
    echo "  - $s"
done
echo "========================================="
echo ""

PASSED=0
FAILED=0
FAILED_SUITES=()

for suite in "${SUITES_TO_RUN[@]}"; do
    cleanup_db
    echo "-----------------------------------------"
    echo "Executing suite: $suite"
    echo "-----------------------------------------"
    if "$SUITES_DIR/$suite"; then
        PASSED=$((PASSED + 1))
        echo "--> SUCCESS: $suite passed"
    else
        FAILED=$((FAILED + 1))
        FAILED_SUITES+=("$suite")
        echo "--> FAILURE: $suite failed"
    fi
    echo ""
done

echo "========================================="
echo "             Test Results                "
echo "========================================="
echo "Passed: $PASSED"
echo "Failed: $FAILED"

if [ "$FAILED" -gt 0 ]; then
    echo "Failed Suites:"
    for fs in "${FAILED_SUITES[@]}"; do
        echo "  - $fs"
    done
    echo "========================================="
    exit 1
fi

echo "All tests passed successfully!"
echo "========================================="
exit 0
