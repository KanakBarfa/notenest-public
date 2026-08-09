# NoteNest AWS Scaling Blueprint & Cost Estimation Matrix

## 1. Overview
This document provides a comprehensive scaling matrix and estimated AWS infrastructure cost for NoteNest across concurrent user tiers ranging from **100 users** to **10 million users**.

---

## 2. Infrastructure Scaling Matrix (100 to 10,000 Users)

| Component / Layer | 100 Users (7 RPS) | 1,000 Users (70 RPS) | 5,000 Users (350 RPS) | 10,000 Users (700 RPS) |
| :--- | :--- | :--- | :--- | :--- |
| **HAProxy / Ingress** | 1 Pod (`c6i.large`) | 2 Pods (`c6i.large`) | 2 Pods (`c6i.large`) | 2 Pods (`c6i.large`) |
| **C++ Note Service** | 1 Pod (`c6i.large`) | 2 Pods (`c6i.large`) | 3 Pods (`c6i.large`) | 3 Pods (`c6i.large`) |
| **Python Auth / User** | 1 Pod (`c6i.large`) | 2 Pods (`c6i.large`) | 2 Pods (`c6i.large`) | 2 Pods (`c6i.large`) |
| **WebSocket Pods** | 1 Pod (`c6i.large`) | 2 Pods (`c6i.large`) | 2 Pods (`c6i.large`) | 2 Pods (`c6i.large`) |
| **Redis Cache** | 1 Node (`cache.t4g.small`)| 2 Nodes (`cache.m6g.large`)| 2 Nodes (`cache.m6g.large`)| 2 Nodes (`cache.m6g.large`)|
| **PostgreSQL Primary** | 1 DB (`db.t4g.medium`) | 1 DB (`db.m6g.large`) | 1 DB (`db.m6g.xlarge`) | 1 DB (`db.m6g.xlarge`) |
| **PostgreSQL Replicas**| 0 Replicas | 1 Replica (`db.m6g.large`)| 2 Replicas (`db.m6g.large`)| 2 Replicas (`db.m6g.large`)|
| **Kafka Brokers** | 1 Broker (`kafka.t3.small`)| 3 Brokers (`kafka.m5.large`)| 3 Brokers (`kafka.m5.large`)| 3 Brokers (`kafka.m5.large`)|
| **EKS Control Plane** | $0.10 / hr | $0.10 / hr | $0.10 / hr | $0.10 / hr |
| **Est. AWS Hourly Cost**| **~$0.35 / hr** | **~$1.20 / hr** | **~$2.10 / hr** | **~$3.20 / hr** |
| **Est. AWS Monthly Cost**| **~$250 / mo** | **~$860 / mo** | **~$1,500 / mo** | **~$2,300 / mo** |

---

## 3. Infrastructure Scaling Matrix (50,000 to 10 Million Users)

| Component / Layer | 50,000 Users (3.5k RPS) | 100,000 Users (7k RPS) | 500,000 Users (35k RPS) | 1,000,000 Users (70k RPS) | 10,000,000 Users (700k RPS) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **HAProxy / Ingress** | 3 Pods (`c6i.xlarge`) | 4 Pods (`c6i.xlarge`) | 10 Pods (`c6i.2xlarge`) | 20 Pods (`c6i.2xlarge`) | 150 Pods (`c6i.2xlarge`) |
| **C++ Note Service** | 8 Pods (`c6i.xlarge`) | 12 Pods (`c6i.xlarge`) | 45 Pods (`c6i.xlarge`) | 100 Pods (`c6i.xlarge`) | 800 Pods (`c6i.xlarge`) |
| **Python Auth / User** | 4 Pods (`c6i.xlarge`) | 8 Pods (`c6i.xlarge`) | 25 Pods (`c6i.xlarge`) | 50 Pods (`c6i.xlarge`) | 400 Pods (`c6i.xlarge`) |
| **WebSocket Pods** | 4 Pods (`c6i.xlarge`) | 6 Pods (`c6i.xlarge`) | 25 Pods (`c6i.xlarge`) | 50 Pods (`c6i.xlarge`) | 400 Pods (`c6i.xlarge`) |
| **Redis Cache Cluster** | 2 Shards (`cache.m6g.xlarge`) | 4 Shards (`cache.m6g.xlarge`) | 12 Shards (`cache.m6g.2xlarge`)| 32 Shards (`cache.m6g.2xlarge`)| 128 Shards (`cache.m6g.2xlarge`)|
| **PostgreSQL Primary** | 1 DB (`db.m6g.2xlarge`) | 1 DB (`db.m6g.2xlarge`) | 1 DB (`db.m6g.8xlarge`) | 1 DB (`db.m6g.16xlarge`) | 4 DB Shards (`db.m6g.16xlarge`)|
| **PostgreSQL Replicas**| 3 Replicas (`db.m6g.xlarge`)| 4 Replicas (`db.m6g.xlarge`)| 10 Replicas (`db.m6g.2xlarge`)| 16 Replicas (`db.m6g.4xlarge`)| 64 Replicas (`db.m6g.4xlarge`)|
| **Kafka Brokers** | 3 Brokers (`kafka.m5.xlarge`)| 3 Brokers (`kafka.m5.xlarge`)| 5 Brokers (`kafka.m5.2xlarge`)| 5 Brokers (`kafka.m5.4xlarge`)| 12 Brokers (`kafka.m5.4xlarge`)|
| **EKS Control Plane** | $0.10 / hr | $0.10 / hr | $0.10 / hr | $0.10 / hr | $0.10 / hr |
| **Est. AWS Hourly Cost**| **~$9.80 / hr** | **~$18.50 / hr** | **~$88.40 / hr** | **~$195.00 / hr** | **~$1,650.00 / hr** |
| **Est. AWS Monthly Cost**| **~$7,050 / mo** | **~$13,320 / mo** | **~$63,650 / mo** | **~$140,400 / mo** | **~$1,188,000 / mo** |

---

## 4. Architectural Scaling Thresholds

1. **100 to 10,000 Users**:
   - Single-cluster EKS deployment with 1 Primary PostgreSQL instance and 1-2 Read Replicas.
   - Standard Redis primary/replica cache setup handles 85%+ of read volume.
2. **50,000 to 1,000,000 Users**:
   - Sharded Redis Cluster (2 to 32 shards) distributing cache load.
   - Up to 16 PostgreSQL Physical Streaming Read Replicas with PgBouncer transaction-mode connection poolers.
   - Kubernetes HPA dynamically scales C++ `note-service` pods from 8 to 100 replicas.
3. **10 Million Users**:
   - Multi-master PostgreSQL sharding (e.g. Citus extension or hash partitioning by `user_id`) to scale write IOPS beyond single-primary limits.
   - Distributed WebSocket edge clusters with global cross-region load balancing.

---

## 5. AWS Cost Sizing Details & Rate Assumptions

- **AWS EC2 Compute (Graviton / x86 On-Demand)**:
  - `c6i.large` (2 vCPU, 4GB RAM): $0.085 / hr
  - `c6i.xlarge` (4 vCPU, 8GB RAM): $0.170 / hr
  - `c6i.2xlarge` (8 vCPU, 16GB RAM): $0.340 / hr
- **AWS RDS PostgreSQL (Graviton2 On-Demand)**:
  - `db.m6g.large` (2 vCPU, 8GB RAM): $0.130 / hr
  - `db.m6g.xlarge` (4 vCPU, 16GB RAM): $0.260 / hr
  - `db.m6g.2xlarge` (8 vCPU, 32GB RAM): $0.520 / hr
  - `db.m6g.8xlarge` (32 vCPU, 128GB RAM): $2.080 / hr
  - `db.m6g.16xlarge` (64 vCPU, 256GB RAM): $4.160 / hr
- **AWS ElastiCache Redis (Graviton2 On-Demand)**:
  - `cache.m6g.large` (2 vCPU, 6.38GB RAM): $0.136 / hr
  - `cache.m6g.xlarge` (4 vCPU, 12.9GB RAM): $0.272 / hr
  - `cache.m6g.2xlarge` (8 vCPU, 26.2GB RAM): $0.544 / hr

*Note: Applying 3-year AWS Savings Plans / Reserved Instances provides approximately 40% to 55% discount over on-demand rates.*
