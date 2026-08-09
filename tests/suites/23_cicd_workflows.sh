#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "[Phase 23] Testing CI/CD GitHub Actions Workflows & Security Automation E2E (v0.23)..."

PROJECT_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
WORKFLOW_DIR="$PROJECT_ROOT/.github/workflows"

echo "1. Verifying GitHub Actions workflow directory structure..."
if [ ! -d "$WORKFLOW_DIR" ]; then
    echo "FAILED: Directory $WORKFLOW_DIR does not exist"
    exit 1
fi

REQUIRED_WORKFLOWS=(
    "ci.yml"
    "cd.yml"
    "security.yml"
)

for wf in "${REQUIRED_WORKFLOWS[@]}"; do
    if [ ! -f "$WORKFLOW_DIR/$wf" ]; then
        echo "FAILED: Required workflow file missing: $wf"
        exit 1
    fi
done
echo "  - All required workflow files verified (ci.yml, cd.yml, security.yml)"

echo "2. Verifying CI/CD architecture documentation..."
if [ ! -f "$PROJECT_ROOT/docs/ci-cd.md" ]; then
    echo "FAILED: Documentation docs/ci-cd.md missing"
    exit 1
fi
echo "  - CI/CD documentation docs/ci-cd.md verified"

echo "3. Validating YAML syntax of workflow configurations..."
PYTHON_BIN="/usr/bin/python3"
if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
    PYTHON_BIN="python3"
fi

"$PYTHON_BIN" -c '
import sys, yaml

files = ["ci.yml", "cd.yml", "security.yml"]
workflow_dir = sys.argv[1]

for f in files:
    path = f"{workflow_dir}/{f}"
    with open(path, "r") as stream:
        try:
            data = yaml.safe_load(stream)
            if not isinstance(data, dict):
                raise ValueError(f"Root object in {f} is not a dict")
            print(f"    - {f} YAML syntax validated successfully")
        except Exception as e:
            print(f"FAILED: YAML parsing error in {f}: {e}")
            sys.exit(1)
' "$WORKFLOW_DIR"
echo "  - All workflow YAML configurations passed syntax validation"

echo "4. Auditing Workflow Jobs and Triggers..."
grep -q "pull_request:" "$WORKFLOW_DIR/ci.yml" || { echo "ci.yml missing pull_request trigger"; exit 1; }
grep -q "lint-code:" "$WORKFLOW_DIR/ci.yml" || { echo "ci.yml missing lint-code job"; exit 1; }
grep -q "unit-and-integration-tests:" "$WORKFLOW_DIR/ci.yml" || { echo "ci.yml missing unit-and-integration-tests job"; exit 1; }
grep -q "helm-and-tf-validation:" "$WORKFLOW_DIR/ci.yml" || { echo "ci.yml missing helm-and-tf-validation job"; exit 1; }
grep -q "docker-e2e-integration:" "$WORKFLOW_DIR/ci.yml" || { echo "ci.yml missing docker-e2e-integration job"; exit 1; }

grep -q "ghcr.io" "$WORKFLOW_DIR/cd.yml" || { echo "cd.yml missing ghcr.io registry reference"; exit 1; }
grep -q "build-and-push-ghcr:" "$WORKFLOW_DIR/cd.yml" || { echo "cd.yml missing build-and-push-ghcr job"; exit 1; }
grep -q "deploy-k3s-helm:" "$WORKFLOW_DIR/cd.yml" || { echo "cd.yml missing deploy-k3s-helm job"; exit 1; }

grep -q "aquasecurity/trivy-action" "$WORKFLOW_DIR/security.yml" || { echo "security.yml missing Trivy scanner"; exit 1; }
grep -q "secret-detection:" "$WORKFLOW_DIR/security.yml" || { echo "security.yml missing secret detection job"; exit 1; }
echo "  - CI/CD workflow triggers, jobs, GHCR registry, and security tools audited successfully"

echo "5. Verifying local C++ code formatting..."
if command -v clang-format >/dev/null 2>&1; then
    clang-format --dry-run --Werror "$PROJECT_ROOT"/src/*.cpp "$PROJECT_ROOT"/include/notenest/*.hpp
    echo "  - C++ formatting check passed cleanly via clang-format"
else
    echo "  - clang-format not installed; skipping dry-run"
fi

echo "6. Validating Helm chart linting..."
if command -v helm >/dev/null 2>&1; then
    helm lint "$PROJECT_ROOT/deployments/helm/notenest" >/dev/null
    echo "  - Helm chart linting succeeded"
else
    echo "  - Helm CLI not installed; skipping helm lint"
fi

echo "7. Validating Terraform module configuration..."
if command -v terraform >/dev/null 2>&1; then
    (cd "$PROJECT_ROOT/deployments/terraform" && terraform init -backend=false >/dev/null && terraform validate >/dev/null)
    echo "  - Terraform module validation succeeded"
else
    echo "  - Terraform CLI not installed; skipping terraform validate"
fi

echo "Phase 23 CI/CD Workflows & Security Automation E2E validation completed successfully!"
