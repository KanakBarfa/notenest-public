# NoteNest System Capacity Model & Scaling Blueprint

## 1. Overview
This capacity model defines resource allocation, throughput limits, and horizontal scaling parameters required to maintain NoteNest latency SLOs (`p99 < 500ms`, availability `> 99.9%`) under increasing Request Per Second (RPS) loads.

## 2. Resource Sizing Metrics

| Metric Tier | 1,000 RPS (Baseline) | 10,000 RPS (Medium) | 50,000 RPS (Peak) |
| :--- | :--- | :--- | :--- |
| API Gateway (Kong/HAProxy) | 2 Replicas (1 vCPU, 1GB RAM) | 6 Replicas (2 vCPU, 2GB RAM) | 20 Replicas (4 vCPU, 4GB RAM) |
| Note Service (C++) | 3 Replicas (1 vCPU, 512MB RAM) | 12 Replicas (2 vCPU, 1GB RAM) | 40 Replicas (4 vCPU, 2GB RAM) |
| Auth / User Service (Python) | 2 Replicas (1 vCPU, 512MB RAM) | 8 Replicas (2 vCPU, 1GB RAM) | 25 Replicas (4 vCPU, 2GB RAM) |
| Redis Cache Cluster | 1 Primary + 1 Replica (2GB RAM) | 3 Shards Cluster (8GB RAM) | 6 Shards Cluster (32GB RAM) |
| PostgreSQL Database | 1 Primary + 2 Replicas (2 vCPU, 4GB RAM) | 1 Primary + 4 Replicas (8 vCPU, 16GB RAM) | 1 Primary + 8 Replicas (32 vCPU, 64GB RAM) |
| Kafka Brokers | 3 Brokers (2 vCPU, 4GB RAM) | 3 Brokers (4 vCPU, 8GB RAM) | 5 Brokers (8 vCPU, 16GB RAM) |

## 3. Scaling Formulae

1. **CPU Allocation**:
   - `Required_Cores = ceil((Total_RPS * Baseline_Latency_ms) / 1000 * Safety_Factor(1.5))`
2. **Database Read Replicas**:
   - `Read_Replicas = ceil((Read_RPS * (1 - Cache_Hit_Ratio(0.85))) / Max_Replica_QPS(2500))`
3. **Redis Cache Memory**:
   - `Memory_GB = (Active_Users * 50KB + Total_Notes * 10KB) * Overdrive_Multiplier(1.3) / 1024 / 1024`

## 4. Bottleneck & Autoscaling Matrix

- **CPU Threshold**: HPA triggers replica increase when Pod CPU exceeds 75% for 2 minutes.
- **PgBouncer Pool Utilization**: Alert triggers when active pooled connections reach 80% capacity.
- **Cache Hit Ratio**: Alert triggers if Redis cache hit ratio drops below 80%.
