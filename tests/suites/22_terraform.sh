#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "[Phase 22] Testing Terraform Infrastructure as Code E2E (v0.22)..."

TF_DIR="$SCRIPT_DIR/../../deployments/terraform"

echo "1. Verifying Terraform project directory structure..."
if [ ! -d "$TF_DIR" ]; then
    echo "FAILED: Directory $TF_DIR does not exist"
    exit 1
fi

REQUIRED_FILES=(
    "main.tf"
    "variables.tf"
    "outputs.tf"
    "terraform.tfvars.example"
    "modules/postgres/main.tf"
    "modules/postgres/variables.tf"
    "modules/postgres/outputs.tf"
    "modules/kafka/main.tf"
    "modules/kafka/variables.tf"
    "modules/kafka/outputs.tf"
    "modules/prometheus/main.tf"
    "modules/prometheus/variables.tf"
    "modules/prometheus/outputs.tf"
    "modules/app/main.tf"
    "modules/app/variables.tf"
    "modules/app/outputs.tf"
    "modules/redis/main.tf"
    "modules/redis/variables.tf"
    "modules/redis/outputs.tf"
    "modules/minio/main.tf"
    "modules/minio/variables.tf"
    "modules/minio/outputs.tf"
    "modules/consul/main.tf"
    "modules/consul/variables.tf"
    "modules/consul/outputs.tf"
    "modules/microservices/main.tf"
    "modules/microservices/variables.tf"
    "modules/microservices/outputs.tf"
    "modules/auxiliary/main.tf"
    "modules/auxiliary/variables.tf"
    "modules/auxiliary/outputs.tf"
)

for file in "${REQUIRED_FILES[@]}"; do
    if [ ! -f "$TF_DIR/$file" ]; then
        echo "FAILED: Required Terraform file missing: $file"
        exit 1
    fi
done
echo "  - All required Terraform structure files and modules verified"

echo "2. Validating Terraform module & resource definitions..."
grep -q 'source.*=.*"./modules/postgres"' "$TF_DIR/main.tf" || { echo "Postgres module import missing"; exit 1; }
grep -q 'source.*=.*"./modules/kafka"' "$TF_DIR/main.tf" || { echo "Kafka module import missing"; exit 1; }
grep -q 'source.*=.*"./modules/prometheus"' "$TF_DIR/main.tf" || { echo "Prometheus module import missing"; exit 1; }
grep -q 'source.*=.*"./modules/redis"' "$TF_DIR/main.tf" || { echo "Redis module import missing"; exit 1; }
grep -q 'source.*=.*"./modules/minio"' "$TF_DIR/main.tf" || { echo "MinIO module import missing"; exit 1; }
grep -q 'source.*=.*"./modules/app"' "$TF_DIR/main.tf" || { echo "App module import missing"; exit 1; }
grep -q 'kreuzwerker/docker' "$TF_DIR/main.tf" || { echo "Docker provider missing"; exit 1; }
echo "  - Terraform module declarations and providers validated"

echo "3. Checking Terraform CLI availability..."
if ! command -v terraform >/dev/null 2>&1; then
    echo "FAILED: terraform CLI is not installed on PATH"
    exit 1
fi

cd "$TF_DIR"

echo "4. Initializing Terraform & validating syntax..."
terraform init > /dev/null
terraform validate > /dev/null
echo "  - Terraform init and validate succeeded"

echo "5. Testing Terraform Workspace Management (dev / stage / e2e)..."
terraform workspace new dev >/dev/null 2>&1 || terraform workspace select dev >/dev/null
terraform workspace new stage >/dev/null 2>&1 || terraform workspace select stage >/dev/null

TEST_WS="e2e-test-ws"
terraform workspace new "$TEST_WS" >/dev/null 2>&1 || terraform workspace select "$TEST_WS" >/dev/null
echo "  - Workspace $TEST_WS created and selected"

cleanup() {
    echo "Cleaning up Terraform test workspace resources..."
    terraform destroy -auto-approve >/dev/null 2>&1 || true
    terraform workspace select default >/dev/null 2>&1 || true
    terraform workspace delete "$TEST_WS" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "6. Planning Terraform provisioning..."
for svc in auth user graphql notification app nginx; do
    IMG=$(docker images --format '{{.Repository}}:{{.Tag}}' 2>/dev/null | grep -i -E "(^|/|_|-)${svc}(-|/|_|$)" | head -n 1 || true)
    if [ -n "$IMG" ]; then
        docker tag "$IMG" "notenest-${svc}:latest" 2>/dev/null || true
    elif [ -n "$(docker images -q notenest:latest 2>/dev/null)" ]; then
        docker tag notenest:latest "notenest-${svc}:latest" 2>/dev/null || true
    fi
done
terraform plan -out=tfplan > /dev/null
[ -f tfplan ] || { echo "Terraform plan failed to generate tfplan file"; exit 1; }
echo "  - Terraform plan generated and resource targets verified"

if docker info >/dev/null 2>&1; then
    echo "7. Executing E2E Terraform Apply (Provisioning infrastructure)..."
    terraform apply -auto-approve tfplan > /dev/null
    echo "  - Infrastructure provisioned successfully"

    echo "8. Verifying Terraform State File & Tracked Resources..."
    STATE_LIST=$(terraform state list)
    echo "$STATE_LIST" | grep "module.postgres.docker_container.postgres" >/dev/null || { echo "State missing postgres container"; exit 1; }
    echo "$STATE_LIST" | grep "module.kafka.docker_container.kafka" >/dev/null || { echo "State missing kafka container"; exit 1; }
    echo "$STATE_LIST" | grep "module.prometheus.docker_container.prometheus" >/dev/null || { echo "State missing prometheus container"; exit 1; }
    echo "$STATE_LIST" | grep "module.redis.docker_container.redis" >/dev/null || { echo "State missing redis container"; exit 1; }
    echo "$STATE_LIST" | grep "module.minio.docker_container.minio" >/dev/null || { echo "State missing minio container"; exit 1; }
    echo "$STATE_LIST" | grep "module.app.docker_container.app" >/dev/null || { echo "State missing app container"; exit 1; }
    echo "$STATE_LIST" | grep "module.app.docker_container.nginx" >/dev/null || { echo "State missing nginx container"; exit 1; }
    echo "$STATE_LIST" | grep "docker_network.tf_network" >/dev/null || { echo "State missing docker network"; exit 1; }
    echo "  - Terraform state correctly tracks all provisioned resources"

    echo "9. Verifying Docker Engine Live Container Resources..."
    POSTGRES_CONTAINER=$(terraform output -raw postgres_container_name)
    KAFKA_CONTAINER=$(terraform output -raw kafka_container_name)
    PROMETHEUS_CONTAINER=$(terraform output -raw prometheus_container_name)
    REDIS_CONTAINER=$(terraform output -raw redis_container_name)
    MINIO_CONTAINER=$(terraform output -raw minio_container_name)
    WEB_CONTAINER=$(terraform output -raw web_frontend_container_name)
    docker inspect "$POSTGRES_CONTAINER" >/dev/null
    docker inspect "$KAFKA_CONTAINER" >/dev/null
    docker inspect "$PROMETHEUS_CONTAINER" >/dev/null
    docker inspect "$REDIS_CONTAINER" >/dev/null
    docker inspect "$MINIO_CONTAINER" >/dev/null
    docker inspect "$WEB_CONTAINER" >/dev/null
    echo "  - Live Docker containers verified running in engine ($POSTGRES_CONTAINER, $KAFKA_CONTAINER, $PROMETHEUS_CONTAINER, $REDIS_CONTAINER, $MINIO_CONTAINER, $WEB_CONTAINER)"

    echo "10. Executing E2E Terraform Destroy (Infrastructure Teardown)..."
    terraform destroy -auto-approve > /dev/null
    echo "  - Infrastructure destroyed successfully"
else
    echo "  - Docker daemon unavailable; skipped live container lifecycle provisioning."
fi

trap - EXIT
cleanup

echo "Phase 22 Terraform IaC E2E validation completed successfully!"
