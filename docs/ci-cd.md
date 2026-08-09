# NoteNest CI/CD & Automated Pipeline Architecture

This document describes the continuous integration, continuous deployment, security scanning, and branch protection specifications for NoteNest (v0.23).

---

## 1. Pipeline Overview

```text
[ Feature Branch PR ]
         │
         ▼
 ┌─────────────────────────────────────────────────────────────┐
 │ GitHub Actions CI Pipeline (ci.yml)                          │
 ├─────────────────────────────────────────────────────────────┤
 │ 1. Code Linting (clang-format, black, flake8, npm)          │
 │ 2. Unit & Service Build Tests (make -j, python py_compile)  │
 │ 3. Helm Chart & Terraform Validation (helm lint, tf validate)│
 │ 4. Docker Compose E2E Integration Test Suite Execution       │
 └──────────────────────────────┬──────────────────────────────┘
                                │
                                ▼
                       [ Branch Protection ]
                   (All status checks pass)
                                │
                                ▼
                        [ Merge to main ]
                                │
         ┌──────────────────────┴──────────────────────┐
         ▼                                             ▼
 ┌──────────────────────────────┐              ┌──────────────────────────────┐
 │ CD Pipeline (cd.yml)         │              │ Security Scanner             │
 ├──────────────────────────────┤              │ (security.yml)               │
 │ 1. Docker Build & Push GHCR  │              ├──────────────────────────────┤
 │ 2. Helm Upgrade Dry-Run K3s  │              │ 1. Trivy FS & Image Scan     │
 └──────────────────────────────┘              │ 2. Secret & Key Audit        │
                                               └──────────────────────────────┘
```

---

## 2. GitHub Actions Workflows

### 2.1 Continuous Integration (`.github/workflows/ci.yml`)
- **Triggers**: Pull requests and direct commits targeting `main`, `master`, and `dev` branches.
- **Jobs**:
  - `lint-code`: Verifies C++ formatting (`clang-format`), Python PEP8 compliance (`black`, `flake8`), and Node.js frontend compilation (`npm run build`).
  - `unit-and-integration-tests`: Compiles C++ monolith binary using GCC 14 with C++26 standard and validates Python microservice syntax.
  - `helm-and-tf-validation`: Validates Kubernetes Helm chart syntax via `helm lint` and verifies Terraform modules via `terraform validate`.
  - `docker-e2e-integration`: Builds Docker images, spins up `docker compose` ephemeral containers, and executes the complete NoteNest E2E integration test suite.

### 2.2 Continuous Deployment (`.github/workflows/cd.yml`)
- **Triggers**: Code merges to `main`/`master` or release tags matching `v*.*.*`.
- **Jobs**:
  - `build-and-push-ghcr`: Authenticates with GitHub Container Registry (`ghcr.io`), builds multi-stage container images, and tags artifacts with Git commit SHA and `latest`.
  - `deploy-k3s-helm`: Executes GitOps deployment by invoking `helm upgrade --install` against the k3s cluster namespace.

### 2.3 Security Scanning (`.github/workflows/security.yml`)
- **Triggers**: Code pushes, pull requests, daily schedule at 02:00 UTC, or manual dispatch.
- **Jobs**:
  - `trivy-fs-scan`: Scans source repository filesystem for known vulnerabilities in static dependencies.
  - `trivy-container-scan`: Scans container images for OS-level vulnerabilities (CRITICAL/HIGH severity).
  - `secret-detection`: Audits repository history to prevent accidental exposure of private keys, AWS credentials, or API tokens.

---

## 3. Branch Protection & Governance Policies

To ensure stability across production deployments, the `main` branch is protected by the following enforcement rules:

1. **Required Status Checks**:
   - `lint-code`
   - `unit-and-integration-tests`
   - `helm-and-tf-validation`
   - `docker-e2e-integration`
2. **Pull Request Approval**: Minimum 1 peer code review required before merging.
3. **No Direct Commits**: Direct force-pushes and branch deletions disabled.
4. **Clean Git History**: Rebase or squash merging enforced to maintain clear linear history.

---

## 4. Artifact Promotion & Container Registry Strategy

NoteNest follows strict artifact immutability:
1. **Build Once**: Images built during the CD workflow are tagged using immutable Git SHAs (`ghcr.io/kanakbarfa/notenest:<git-sha>`).
2. **Promote via Tags**: After staging validation succeeds, the same SHA tag is updated to `latest` or semver release tags without rebuilding binary layers.
3. **Helm Values Sync**: Deployment manifests update container image tags dynamically during `helm upgrade`.
