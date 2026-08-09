# NoteNest: Cloud-Native Microservices Architecture Platform

NoteNest is an enterprise-grade, distributed collaborative document and notes platform. Built with a high-throughput C++ core engine, Python microservices, and React frontend, NoteNest demonstrates production-ready microservice design, transactional outbox event streaming, real-time CRDT document synchronization, physical database replication, multi-tier network security, full OpenTelemetry observability, Kubernetes/Helm orchestration, Terraform IaC, and chaos resilience engineering.

---

## Architecture Overview & HLD Topology

```
+-----------------------------------------------------------------------------------+
|                                  CLIENT TIER                                      |
|            Browser (React SPA / CodeMirror 6)  |  Mobile / API Clients            |
+----------------------------------------+------------------------------------------+
                                         |
                                         v
+-----------------------------------------------------------------------------------+
|                             EDGE & SECURITY NETWORK                               |
|          HAProxy Load Balancer (L4/L7)  -->  Kong API Gateway (Rate Limiting / JWT) |
+----------------------------------------+------------------------------------------+
                                         |
                                         v
+-----------------------------------------------------------------------------------+
|                             MICROSERVICES MESH TIER                               |
|   +-------------------+   +--------------------+   +---------------------------+  |
|   | Note Service      |   | Auth Service       |   | User Service              |  |
|   | (C++ Epoll/gRPC)  |   | (Python / gRPC)    |   | (Python / gRPC)           |  |
|   +---------+---------+   +---------+----------+   +-------------+-------------+  |
|             |                       |                            |                |
|             +-----------------------+----------------------------+                |
|                                     |                                             |
|                           Consul Service Discovery                                |
+-------------------------------------+---------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------------+
|                             PERSISTENCE & EVENT TIER                              |
|   PostgreSQL Primary  <--->  PostgreSQL Standby Read Replicas (Streaming Rep)     |
|   PgBouncer Pooler (Write: 6432)  |  PgBouncer Pooler (Read: 6433)                |
|   Redis Cache-Aside Cluster       |  Apache Kafka & Outbox Relay (Events)         |
|   MinIO S3 Object Storage         |  RabbitMQ Queue (PDF Generation Worker)       |
+-----------------------------------------------------------------------------------+
```

---

## Technical Stack & Key Capabilities

- **High-Performance Backend**: Written in C++26 using a Linux `epoll` non-blocking socket router, multithreaded worker pool, and low-overhead HTTP/gRPC handlers.
- **Inter-Service Communication**: gRPC with Protocol Buffer v3 schemas (`auth.proto`, `user.proto`, `note.proto`, `attachment.proto`) over HTTP/2 transport.
- **Real-Time Collaboration**: WebSockets (RFC 6455) relaying binary Yjs CRDT document deltas, CodeMirror 6 state synchronization, and user presence awareness.
- **Transactional Outbox & Event Streaming**: Dual-phase Outbox pattern in C++ (`PgNoteRepository` + `OutboxRelay`) streaming domain events to Apache Kafka with Python notification consumers and RabbitMQ PDF export workers.
- **Database Scalability**: PostgreSQL 15 Physical Streaming Replication (1 Primary + 2 Standby Replicas) with PgBouncer transaction-mode connection pooling (`6432` write, `6433` read) and lag-aware read/write query splitting.
- **Dual Caching**: Redis out-of-process cache-aside with key-level mutex double-checked locking for stampede protection.
- **API Gateway & CDN**: Kong API Gateway handling Redis token-bucket rate limiting (60/min anon, 600/min auth) and JWT HMAC-SHA256 offloading. Nginx edge caching with AWS SigV4 presigned S3 URLs.
- **Full Observability**: Prometheus metrics, Loki log aggregation, Tempo distributed tracing with W3C traceparent context propagation across HTTP, gRPC, and Kafka, OpenTelemetry Collector, and Grafana dashboards.
- **4-Tier Network Segmentation**: Docker CIDR subnets (`public_net`, `app_net`, `data_net`, `mgmt_net`) enforcing minimum-privilege tier isolation with an Alpine Bastion admin node.
- **Infrastructure as Code & Orchestration**: Modular Helm 3 charts (`deployments/helm/notenest`), Terraform IaC modules (`deployments/terraform`), and GitHub Actions CI/CD pipelines (`ci.yml`, `cd.yml`, `security.yml`).
- **Resilience Engineering**: k6 performance test suite (`tests/k6/`) and automated chaos experiment scripts (`scripts/chaos_experiments.sh`).

---

## Project Structure

```
.
├── src/                    # C++ Core Engine (epoll router, gRPC clients/services, Outbox)
├── include/notenest/       # C++ Caching, DB pool, Circuit Breakers, EventBus headers
├── services/               # Python Microservices (auth, user, notification), GraphQL Server
├── frontend/               # React (Vite) SPA UI (CodeMirror 6, Yjs CRDT, glassmorphism)
├── protos/                 # Shared Protocol Buffer definitions
├── db/                     # PostgreSQL database migration scripts (001_init to 007_outbox)
├── deployments/            # Helm charts, Terraform modules, HAProxy, Nginx, Prometheus
├── scripts/                # Chaos experiment scripts and entrypoint wrappers
├── tests/                  # End-to-end integration test runner, suites, and k6 scripts
└── docs/                   # Operational Runbooks, Capacity Model, and Infrastructure Sizing
```

---

## Quickstart (Local Docker Stack)

### Prerequisites
- Docker Engine 24+ & Docker Compose v2+
- Make & C++20/26 compiler (for local builds)

### 1. Launch Ephemeral Infrastructure
```bash
docker compose up -d --build
```

### 2. Access Platform Endpoints
- **React Frontend**: `http://localhost:8081`
- **HAProxy Load Balancer**: `http://localhost:80`
- **Kong API Gateway**: `http://localhost:8000`
- **GraphQL Apollo Gateway**: `http://localhost:4000/graphql`
- **Consul UI**: `http://localhost:8500`
- **Grafana Dashboard**: `http://localhost:3000` (admin/admin)
- **MinIO S3 Console**: `http://localhost:9001` (minioadmin/minioadmin)

---

## Execution of Automated Validation Suite

NoteNest includes a smart change-detecting integration test runner that executes 24 automated test suites covering all microservice tiers, fault tolerance, and security boundaries:

```bash
# Run all end-to-end integration and chaos validation suites
./tests/run_tests.sh --all

# Run specific suite (e.g. gRPC microservices or chaos load testing)
./tests/run_tests.sh 14_microservices_grpc
./tests/run_tests.sh 24_chaos_load_testing
```

---

## Documentation & Production Operations

- **Operational Runbooks**: [`docs/runbooks/`](docs/runbooks/)
  - `RUNBOOK-001`: High Error Rate & p99 Latency SLO Breach Triage
  - `RUNBOOK-002`: Kafka Consumer Lag & Outbox Relay Backpressure Recovery
  - `RUNBOOK-003`: Kubernetes Pod CrashLoopBackOff & OOMKilled Triage
- **Capacity Sizing & AWS Scaling Cost Matrix**: [`docs/capacity-model.md`](docs/capacity-model.md) & [`docs/aws-scaling-cost-matrix.md`](docs/aws-scaling-cost-matrix.md)

---

## License

NoteNest is open-source software licensed under the [MIT License](LICENSE).
