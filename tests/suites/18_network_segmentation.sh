#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "[Phase 18] Testing VPC & Network Segmentation..."

# Expected CIDR ranges for each network tier
declare -A EXPECTED_SUBNETS=(
    ["public_net"]="172.20.0.0/24"
    ["app_net"]="172.20.1.0/24"
    ["data_net"]="172.20.2.0/24"
    ["mgmt_net"]="172.20.3.0/24"
)

echo "1. Verifying Docker network existence and CIDR ranges..."
for net in public_net app_net data_net mgmt_net; do
    SUBNET=$(docker network inspect "${net}" --format '{{range .IPAM.Config}}{{.Subnet}}{{end}}' 2>/dev/null \
        || docker network inspect "notenest_${net}" --format '{{range .IPAM.Config}}{{.Subnet}}{{end}}' 2>/dev/null \
        || docker network inspect "notenest-public_${net}" --format '{{range .IPAM.Config}}{{.Subnet}}{{end}}' 2>/dev/null \
        || docker network inspect "notenest_public_${net}" --format '{{range .IPAM.Config}}{{.Subnet}}{{end}}' 2>/dev/null \
        || docker network inspect $(docker network ls --format '{{.Name}}' 2>/dev/null | grep -E "(^|_)${net}$" | head -n 1) --format '{{range .IPAM.Config}}{{.Subnet}}{{end}}' 2>/dev/null \
        || echo "NOT_FOUND")
    SUBNET=$(echo "$SUBNET" | xargs)
    if [ "$SUBNET" == "NOT_FOUND" ]; then
        echo "FAILED: Network $net does not exist"
        exit 1
    fi
    if [ "$SUBNET" != "${EXPECTED_SUBNETS[$net]}" ]; then
        echo "FAILED: Network $net has subnet $SUBNET, expected ${EXPECTED_SUBNETS[$net]}"
        exit 1
    fi
    echo "  - $net: $SUBNET (correct)"
done

echo "2. Verifying network isolation - HAProxy (public_net) cannot reach DB (data_net)..."
if docker exec notenest-haproxy-container sh -c \
    'apk add --no-cache iputils > /dev/null 2>&1; ping -c 1 -W 2 db 2>&1'; then
    echo "FAILED: HAProxy can reach DB -- network isolation is broken"
    exit 1
fi
echo "  - HAProxy correctly CANNOT reach DB (network isolation verified)"

echo "3. Verifying network isolation - Nginx (public_net + app_net) cannot reach DB (data_net)..."
if docker exec notenest-nginx-container sh -c \
    'apt-get update > /dev/null 2>&1; apt-get install -y iputils-ping > /dev/null 2>&1; ping -c 1 -W 2 db 2>&1'; then
    echo "FAILED: Nginx can reach DB -- network isolation is broken"
    exit 1
fi
echo "  - Nginx correctly CANNOT reach DB (network isolation verified)"

echo "4. Verifying app-to-data connectivity - App can reach PgBouncer..."
if ! docker exec notenest-app-container sh -c \
    'apt-get update > /dev/null 2>&1; apt-get install -y iputils-ping > /dev/null 2>&1; ping -c 1 -W 2 pgbouncer 2>&1'; then
    echo "FAILED: App cannot reach PgBouncer -- app-to-data connectivity is broken"
    exit 1
fi
echo "  - App can reach PgBouncer on data_net (connectivity verified)"

echo "5. Verifying bastion can reach all network tiers..."
BASTION_SETUP="apk add --no-cache iputils > /dev/null 2>&1"
docker exec notenest-bastion-container sh -c "$BASTION_SETUP" 2>/dev/null || true

for target in haproxy app redis consul; do
    if ! docker exec notenest-bastion-container sh -c "ping -c 1 -W 2 $target 2>&1"; then
        echo "FAILED: Bastion cannot reach $target"
        exit 1
    fi
    echo "  - Bastion can reach $target"
done
echo "  - Bastion has connectivity to all 4 network tiers"

echo "6. Verifying end-to-end API through segmented network (public_net -> app_net -> data_net)..."
USER_EMAIL="netseg_test_${RANDOM}@example.com"
USER_PASS="Password123!"

SIGNUP_STATUS=$(curl -s -w "%{http_code}" -o /dev/null -X POST "$SERVER_URL/signup" \
    -H "Content-Type: application/json" \
    -d "{\"email\": \"$USER_EMAIL\", \"password\": \"$USER_PASS\"}")
if [ "$SIGNUP_STATUS" -ne 201 ]; then
    echo "FAILED: Signup through segmented network returned HTTP $SIGNUP_STATUS (expected 201)"
    exit 1
fi
echo "  - Signup succeeded (public_net -> app_net -> data_net)"

LOGIN_RESP=$(curl -s -X POST "$SERVER_URL/login" \
    -H "Content-Type: application/json" \
    -d "{\"email\": \"$USER_EMAIL\", \"password\": \"$USER_PASS\"}")
TOKEN=$(echo "$LOGIN_RESP" | jq -r '.token // empty')
if [ -z "$TOKEN" ] || [ "$TOKEN" == "null" ]; then
    echo "FAILED: Login through segmented network failed: $LOGIN_RESP"
    exit 1
fi
echo "  - Login succeeded, token acquired"

CREATE_NOTE_RESP=$(curl -s -X POST "$SERVER_URL/notes" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"title\": \"Network Segmentation Test\", \"content\": \"Verifying full chain through VPC tiers\"}")
NOTE_ID=$(echo "$CREATE_NOTE_RESP" | jq -r '.id // empty')
if [ -z "$NOTE_ID" ] || [ "$NOTE_ID" == "null" ]; then
    echo "FAILED: Create note through segmented network failed: $CREATE_NOTE_RESP"
    exit 1
fi
echo "  - Note created (ID: $NOTE_ID) -- full public_net -> app_net -> data_net chain verified"

echo "[Phase 18] Network Segmentation tests passed successfully!"
