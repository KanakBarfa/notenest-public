# NoteNest System Architecture & Component Topology

This document describes the high-level and low-level system architecture of NoteNest as of Phase 22 (v0.22).

---

## 1. System Topology & Network Segmentation

NoteNest utilizes containerized microservices segmented across four Docker networks for defense-in-depth isolation: `public_net` (172.20.0.0/24) for internet-facing edge services (HAProxy, Nginx, Edge-Nginx), `app_net` (172.20.1.0/24) for application microservices (Kong, App, Auth, User, GraphQL, Notification, Consul), `data_net` (172.20.2.0/24) for data stores and message brokers (Primary DB, Read Replicas, Write/Read PgBouncers, Redis, MinIO, Kafka, RabbitMQ), and `mgmt_net` (172.20.3.0/24) for management and observability (Consul, Bastion). HAProxy provides external L7 round-robin load balancing and consistent path hashing for WebSockets. HashiCorp Consul provides dynamic service registry and health-checked service discovery for gRPC load balancing across all internal services.

```mermaid
graph TD
    subgraph ClientLayer["Client Layer"]
        Browser["Web Browser / Client Applications"]
    end

    subgraph PublicNet["Public Network (public_net - 172.20.0.0/24)"]
        HAProxy["External HAProxy LB<br/>Port 80 (Round-Robin + WS Consistent Hashing)"]
        NginxProxy["Nginx Main Proxy<br/>Port 8082"]
        EdgeCDN["Edge Nginx CDN Node<br/>Port 8081<br/>/var/cache/nginx/cdn"]
    end

    subgraph AppNet["Application Network (app_net - 172.20.1.0/24)"]
        KongGateway["Kong API Gateway<br/>Port 8000<br/>DB-less declarative mode"]
        Consul["HashiCorp Consul<br/>Port 8500 (HTTP API / DNS)<br/>Service Registry & Discovery"]
        AuthService["Python Auth Service<br/>Port 50051 (gRPC)<br/>Argon2id + JWT Token Engine"]
        UserService["Python User Service<br/>Port 50052 (gRPC)<br/>User Profile Querying"]
        NoteService["C++ Note Service<br/>Port 50053/50054 (gRPC) & Port 8080 (REST/SSE/WS)<br/>Note CRUD + Yjs WebSockets + OutboxRelay"]
        AttService["C++ Attachment Service<br/>Port 50054 (gRPC)<br/>S3 Presigned URLs + Quota"]
        GraphQL["GraphQL Microservice<br/>Port 4000<br/>Apollo Server 4 + DataLoader"]
        NotificationService["Python Notification & PDF Worker<br/>Kafka Consumer + RabbitMQ Worker"]
    end

    subgraph DataNet["Data Network (data_net - 172.20.2.0/24)"]
        PgBouncerWrite["PgBouncer Write Pooler<br/>Port 6432 (Primary Routing)"]
        PgBouncerRead["PgBouncer Read Pooler<br/>Port 6433 (Replica Round-Robin Routing)"]
        Redis["Redis Cache<br/>Port 6379<br/>Cache-aside + Gateway Limits + Idempotency"]
        DBPrimary[("PostgreSQL 15 Primary DB<br/>Port 5432 (WAL Level: Replica)")]
        DBReplica1[("PostgreSQL 15 Read Replica 1<br/>Port 5432 (Hot Standby)")]
        DBReplica2[("PostgreSQL 15 Read Replica 2<br/>Port 5432 (Hot Standby)")]
        OriginNginx["Origin Nginx Gateway<br/>Port 9005"]
        MinIO[("MinIO S3 Storage<br/>Ports 9000 / 9001")]
        KafkaBroker["Kafka KRaft Broker<br/>Port 9092<br/>note.events & DLT"]
        RabbitMQBroker["RabbitMQ Broker<br/>Port 5672 / 15672<br/>pdf.requests RPC queue"]
    end

    subgraph MgmtNet["Management Network (mgmt_net - 172.20.3.0/24)"]
        Bastion["Alpine Bastion Host<br/>Cross-network admin access"]
    end

    AuthService -->|"Register & TTL Heartbeat"| Consul
    UserService -->|"Register & TTL Heartbeat"| Consul
    NoteService -->|"Register & TTL Heartbeat"| Consul
    AttService -->|"Register & TTL Heartbeat"| Consul
    NotificationService -->|"Register & TTL Heartbeat"| Consul
    GraphQL -->|"Register & TTL Heartbeat"| Consul

    NoteService -->|"Consul Service Discovery"| Consul
    NotificationService -->|"Consul Service Discovery"| Consul

    Browser -->|HTTP GET/POST /api| HAProxy
    Browser -->|HTTP SSE /api/events| HAProxy
    Browser -->|WebSocket /api/notes/:id/ws| HAProxy
    Browser -->|GraphQL /api/graphql| HAProxy
    Browser -->|HTTP PUT/GET Attachments| EdgeCDN

    HAProxy -->|"Round-Robin / WS Hashing"| NginxProxy
    NginxProxy -->|"Proxy /api/* (HTTP/SSE/WS/GraphQL)"| KongGateway
    NginxProxy -->|Proxy /notenest-attachments/*| EdgeCDN

    KongGateway -->|Redis Rate Limits & JWT Offload| Redis
    KongGateway -->|Forward Valid REST / SSE / WS| NoteService
    KongGateway -->|Forward /graphql Requests| GraphQL

    EdgeCDN -->|Cache Miss Proxy| OriginNginx
    OriginNginx -->|S3 Protocol| MinIO

    NoteService -->|"gRPC (VerifyToken/Signup/Login)"| AuthService
    NoteService -->|"gRPC (GetUserProfile)"| UserService
    NoteService -->|"gRPC (GenerateUploadUrl)"| AttService
    NoteService -->|"Transactional Outbox Insert"| PgBouncerWrite
    NoteService -->|"Produce note.events (librdkafka)"| KafkaBroker
    NoteService -->|"RabbitMQ RPC pdf.requests (librabbitmq)"| RabbitMQBroker

    KafkaBroker -->|"Subscribe note.events"| NotificationService
    RabbitMQBroker -->|"Consume pdf.requests"| NotificationService
    NotificationService -->|"gRPC Notify (Port 50054)"| NoteService

    AuthService -->|"Write SQL"| PgBouncerWrite
    AuthService -->|"Read SQL (Fallback Primary)"| PgBouncerRead
    UserService -->|"Read SQL (Fallback Primary)"| PgBouncerRead
    NoteService -->|"Mutations SQL"| PgBouncerWrite
    NoteService -->|"SELECT Queries (Fallback Primary)"| PgBouncerRead
    GraphQL -->|"Mutations"| PgBouncerWrite
    GraphQL -->|"Queries (Fallback Primary)"| PgBouncerRead

    PgBouncerWrite -->|"Write Transactions"| DBPrimary
    PgBouncerRead -->|"Read Transactions"| DBReplica1
    PgBouncerRead -->|"Read Transactions"| DBReplica2
    DBPrimary ==>|"WAL Streaming Physical Replication"| DBReplica1
    DBPrimary ==>|"WAL Streaming Physical Replication"| DBReplica2

    Bastion -.->|"Admin Access"| HAProxy
    Bastion -.->|"Admin Access"| NoteService
    Bastion -.->|"Admin Access"| Redis
    Bastion -.->|"Admin Access"| Consul
```

---

## 2. gRPC Microservice Communication & Tracing Flow

Internal services communicate via Protobuf RPCs with gRPC metadata carrying `trace-id` context. Token verification on the per-request hot path is done locally in C++ using HMAC-SHA256 (Kong has already verified the signature upstream; local verify is ~5-10μs). The Python Auth gRPC service is only called for `signup` and `login` (Argon2id password hashing).

```mermaid
sequenceDiagram
    autonumber
    actor Client
    participant Kong as Kong API Gateway (8000)
    participant NoteSvc as C++ Note Service (50053 / 8080)
    participant AuthSvc as Python Auth Service (50051)
    participant UserSvc as Python User Service (50052)
    participant DB as PostgreSQL DB (5432 via PgBouncer)

    Client->>Kong: POST /api/notes (Title, Content, Bearer JWT)
    Kong->>Kong: Rate limit check & JWT signature verification (HMAC-SHA256)
    Kong->>NoteSvc: Forward request + x-trace-id header

    NoteSvc->>NoteSvc: Local HMAC-SHA256 JWT verify (~5-10μs, no network)
    Note over NoteSvc: Python Auth gRPC VerifyToken removed from hot path

    NoteSvc->>UserSvc: gRPC GetUserProfile(user_id) [metadata: trace-id]
    UserSvc->>DB: SELECT id, email FROM users WHERE id = %s::uuid
    DB-->>UserSvc: User Record
    UserSvc-->>NoteSvc: UserProfileResponse(email, created_at)

    NoteSvc->>DB: INSERT INTO notes ... RETURNING id
    DB-->>NoteSvc: Created Note
    NoteSvc-->>Kong: 201 Created (Note JSON)
    Kong-->>Client: 201 Created
```

---

## 3. API & Database Request Flow (Notes & Auth)

Authentication uses Argon2id password hashing and OpenSSL HMAC-SHA256 JWT tokens offloaded to Kong API Gateway. Ingress traffic is checked against Redis rate-limiting counters. Note queries follow the **Cache-Aside** pattern with key-level locking for stampede protection.

```mermaid
sequenceDiagram
    autonumber
    actor Client
    participant Nginx as Main Nginx (Port 80)
    participant Kong as Kong Gateway (Port 8000)
    participant App as C++ App (Port 8080)
    participant Redis as Redis Cache (6379)
    participant DB as PostgreSQL DB (5432)

    Client->>Nginx: HTTP GET /api/notes
    Nginx->>Kong: Forward request to /notes
    Kong->>Redis: Check & Increment Rate Limit Counter
    
    alt Rate Limit Exceeded
        Kong-->>Nginx: 429 Too Many Requests (Retry-After)
        Nginx-->>Client: 429 Too Many Requests
    else Allowed & Valid JWT Token
        Kong->>Kong: Verify Bearer JWT Signature & Expiry
        Kong->>App: Forward Valid Request
        App->>Redis: GET user:{owner_id}:notes
        
        alt Cache HIT
            Redis-->>App: Return cached JSON array
        else Cache MISS
            App->>Redis: Acquire Key Lock (user:{owner_id}:notes:lock)
            App->>DB: Query notes & note_shares tables by owner_id
            DB-->>App: Return database records
            App->>Redis: SET user:{owner_id}:notes JSON
            App->>Redis: Release Key Lock
        end

        App-->>Kong: 200 OK (JSON Notes List)
        Kong-->>Nginx: 200 OK
        Nginx-->>Client: 200 OK
    end
```

---

## 4. Attachment Upload Flow (Presigned S3 PUT)

Attachment uploads use custom S3 AWS Signature Version 4 presigned URLs.

```mermaid
sequenceDiagram
    autonumber
    actor Client
    participant App as C++ App (Port 8080)
    participant Edge as Edge Nginx (Port 8081)
    participant Origin as Origin Nginx (Port 9005)
    participant S3 as MinIO S3 (Port 9000)

    Client->>App: POST /api/notes/:id/attachments (filename)
    App->>App: Generate S3 SigV4 Presigned PUT URL
    App-->>Client: 200 OK (attachment_id, key, url: http://localhost:8081/...)

    Client->>Edge: PUT /notenest-attachments/key (Binary payload)
    Edge->>Origin: Pass-through non-cacheable PUT
    Origin->>S3: Write object payload to bucket
    S3-->>Origin: 200 OK
    Origin-->>Edge: 200 OK
    Edge-->>Client: 200 OK

    Client->>App: POST /api/notes/:id/attachments/complete
    App->>App: Enforce 100MB User Quota Check
    App-->>Client: 201 Created
```

---

## 5. CDN Attachment Download & Edge Caching Flow

CDN Edge caching operates on disk (`/var/cache/nginx/cdn`) using proxy cache keys (`$scheme$proxy_host$uri`).

```mermaid
sequenceDiagram
    autonumber
    actor Client
    participant Edge as Edge Nginx (Port 8081)
    participant Origin as Origin Nginx (Port 9005)
    participant S3 as MinIO S3 (Port 9000)

    Client->>Edge: GET /notenest-attachments/key?...
    Edge->>Edge: Lookup /var/cache/nginx/cdn key

    alt Cache HIT (Subsequent Requests)
        Edge-->>Client: 200 OK (X-Cache-Status: HIT, Cache-Control: public, max-age=3600)
    else Cache MISS (First Request)
        Edge->>Origin: Forward GET request
        Origin->>S3: Fetch object from bucket
        S3-->>Origin: 200 OK (Binary Data + ETag)
        Origin-->>Edge: 200 OK (Cache-Control: public, max-age=3600)
        Edge->>Edge: Save file to /var/cache/nginx/cdn
        Edge-->>Client: 200 OK (X-Cache-Status: MISS, Cache-Control: public, max-age=3600)
    end
```

---

## 6. Real-Time Server-Sent Events (SSE) & Note Sharing Flow

Server-Sent Events (`text/event-stream`) provide real-time notification streams. Subscriptions connect via `GET /api/events?token=...`, managed by `EventBus`.

```mermaid
sequenceDiagram
    autonumber
    actor UserB as User B (Recipient)
    actor UserA as User A (Sender)
    participant Nginx as Main Nginx (Port 80)
    participant Kong as Kong Gateway (Port 8000)
    participant App as C++ App (Port 8080)
    participant EventBus as EventBus Engine
    participant DB as PostgreSQL DB (5432)

    UserB->>Nginx: GET /api/events?token=JWT_B
    Nginx->>Kong: Forward /events?token=JWT_B (proxy_buffering off)
    Kong->>Kong: Authenticate JWT Token
    Kong->>App: Forward persistent HTTP stream (fd_B)
    App->>EventBus: subscribe(user_id_B, fd_B)
    App-->>UserB: HTTP 200 text/event-stream (Connection: keep-alive)

    loop Heartbeat (Every 15s)
        EventBus->>UserB: Send ": keep-alive\n\n"
    end

    UserA->>Nginx: POST /api/notes/:id/share (target_email: UserB)
    Nginx->>Kong: Forward share request
    Kong->>App: Forward request
    App->>DB: Insert into note_shares table
    App->>EventBus: publish(user_id_B, note_shared_json)
    EventBus->>UserB: Stream "data: {\"type\":\"note_shared\",\"note_id\":...}\n\n"
    App-->>UserA: 200 OK {"status":"success"}
```

---

## 7. Real-Time WebSockets & Yjs CRDT Collaboration Flow

WebSockets (RFC 6455) enable live multi-user CRDT collaboration, remote cursor tracking, and presence events per note room (`note:{id}`), managed by `RoomRegistry` and Yjs + CodeMirror 6.

```mermaid
sequenceDiagram
    autonumber
    actor UserA as User A (Editor)
    actor UserB as User B (Editor)
    participant Nginx as Main Nginx (Port 80)
    participant Kong as Kong Gateway (Port 8000)
    participant App as C++ App (Port 8080)
    participant RoomReg as RoomRegistry Engine

    UserA->>Nginx: GET /api/notes/:id/ws?token=JWT_A (Upgrade: websocket)
    Nginx->>Kong: Forward WS Upgrade Request
    Kong->>Kong: Verify Query JWT Token & Extract User A
    Kong->>App: Forward HTTP GET Upgrade
    App->>RoomReg: joinRoom(note_id, fd_A, user_id_A, email_A)
    App-->>UserA: HTTP 101 Switching Protocols (Sec-WebSocket-Accept)

    UserB->>Nginx: GET /api/notes/:id/ws?token=JWT_B (Upgrade: websocket)
    Nginx->>Kong: Forward WS Upgrade Request
    Kong->>App: Forward HTTP GET Upgrade
    App->>RoomReg: joinRoom(note_id, fd_B, user_id_B, email_B)
    App-->>UserB: HTTP 101 Switching Protocols
    RoomReg->>UserA: Broadcast WS Frame {"type":"presence","event":"user_joined",...}

    UserA->>App: WS Binary Frame (Opcode 0x02: Yjs Sync Step 1 / Updates)
    App->>RoomReg: getRelayTargets(fd_A)
    RoomReg->>UserB: Relay Binary Frame (Opcode 0x02) to Room Participants

    UserB->>App: WS Binary Frame (Opcode 0x02: Yjs Awareness / Remote Cursors)
    App->>RoomReg: getRelayTargets(fd_B)
    RoomReg->>UserA: Relay Binary Frame (Opcode 0x02) to Room Participants
```

---

## 8. GraphQL Layer & DataLoader Batching Flow

The GraphQL microservice eliminates multiple REST client round-trips by allowing clients to fetch Note, Author, and Comments in a single request. DataLoader solves the N+1 database query problem by batching individual ID lookups into single SQL `ANY($1::uuid[])` queries.

```mermaid
sequenceDiagram
    autonumber
    actor Client as Mobile / Web Client
    participant Nginx as Main Nginx (Port 80)
    participant Kong as Kong Gateway (Port 8000)
    participant GQL as GraphQL Service (Port 4000)
    participant Loader as DataLoader Engine
    participant DB as PostgreSQL 15 DB

    Client->>Nginx: POST /api/graphql (Query: note(id) + author + comments)
    Nginx->>Kong: Proxy request to /graphql
    Kong->>Kong: Verify JWT token & rate limits
    Kong->>GQL: Forward GraphQL POST request
    GQL->>DB: Query note record by ID
    GQL->>Loader: Queue user_id for author & note_id for comments
    Loader->>DB: SELECT * FROM users WHERE id = ANY($1::uuid[])
    Loader->>DB: SELECT * FROM comments WHERE note_id = ANY($1::uuid[])
    Loader-->>GQL: Resolve batched promises
    GQL-->>Client: Single JSON Response (Note + Author + Comments)
```

---

## 9. Kafka Event Bus, Outbox Pattern, & Message Queue Architecture (v0.15)

Phase 15 introduces durable asynchronous event streaming via Kafka, transactional event dispatch via the Outbox Pattern, and point-to-point RPC via RabbitMQ.

```mermaid
graph TD
    subgraph OriginNetwork["Origin Network (origin_net)"]
        NoteService["C++ Note Service<br/>Port 8080 / 50053 / 50054"]
        OutboxRelay["Outbox Relay<br/>(Background thread in C++ app)"]
        Kafka["Kafka KRaft Broker<br/>Port 9092<br/>Topics: note.events, note.events.DLT"]
        NotificationConsumer["Python Notification Service<br/>Kafka Consumer + RabbitMQ Worker"]
        RabbitMQ["RabbitMQ<br/>Port 5672 / 15672<br/>Queues: pdf.requests, pdf.replies"]
        PgBouncer["PgBouncer<br/>Port 6432"]
        DB[("PostgreSQL 15 DB<br/>notes & outbox tables")]
        Redis["Redis Cache<br/>Port 6379<br/>Idempotency keys: notif:processed:*"]
    end

    NoteService -->|"INSERT notes + outbox<br/>(Single DB Transaction)"| PgBouncer
    PgBouncer --> DB
    OutboxRelay -->|"Poll outbox table (SKIP LOCKED)<br/>every 500ms"| PgBouncer
    OutboxRelay -->|"Produce note.events<br/>(librdkafka)"| Kafka
    Kafka -->|"Consume note.events<br/>consumer group: notification-group"| NotificationConsumer
    NotificationConsumer -->|"Idempotency Check"| Redis
    NotificationConsumer -->|"gRPC Notify(user_id, payload)"| NoteService
    NoteService -->|"SSE push to client"| Browser
    Kafka -->|"Poison Messages (3 retries)"| DLT["note.events.DLT"]

    NoteService -->|"Produce pdf.requests<br/>(librabbitmq)"| RabbitMQ
    RabbitMQ -->|"Consume pdf.requests"| NotificationConsumer
    NotificationConsumer -->|"Publish pdf.replies<br/>(correlation_id match)"| RabbitMQ
    RabbitMQ -->|"Return PDF Summary JSON"| NoteService
```

---

## 10. Observability Architecture: Logs, Metrics, Traces (v0.19)

Phase 19 integrates full 360° observability across all microservices using the Grafana LGTM stack (Loki, Grafana, Tempo, Prometheus) plus OpenTelemetry Collector and Alertmanager.

```mermaid
graph TD
    subgraph Microservices["Microservices Tier (app_net)"]
        App["C++ Note Service (app)<br/>Port 8080 /metrics"]
        Auth["Python Auth Service (auth)<br/>Port 9101 /metrics"]
        User["Python User Service (user)<br/>Port 9102 /metrics"]
        Notif["Python Notification Service<br/>Port 9103 /metrics"]
        GraphQL["GraphQL Service<br/>Port 4000 /metrics"]
    end

    subgraph ObservabilityStack["Observability & Management Tier (mgmt_net / app_net)"]
        Prometheus["Prometheus (Port 9090)<br/>Scrapes /metrics every 5s"]
        Alertmanager["Alertmanager (Port 9093)<br/>Alert rules: HighErrorRate (>1%)"]
        Promtail["Promtail Agent<br/>Tails Docker container JSON stdouts"]
        Loki["Loki Log Store (Port 3100)<br/>Indexes by container, service, trace_id"]
        OTELCol["OTEL Collector (Ports 4317/4318)<br/>OTLP gRPC/HTTP receiver"]
        Tempo["Tempo Trace Store (Port 3200)<br/>Distributed waterfall trace spans"]
        Grafana["Grafana Dashboards (Port 3000)<br/>Single-pane view for Metrics, Logs, Traces"]
    end

    App -->|Prometheus Metrics| Prometheus
    Auth -->|Prometheus Metrics| Prometheus
    User -->|Prometheus Metrics| Prometheus
    Notif -->|Prometheus Metrics| Prometheus
    GraphQL -->|Prometheus Metrics| Prometheus

    App -->|stdout JSON logs| Promtail
    Auth -->|stdout JSON logs| Promtail
    User -->|stdout JSON logs| Promtail
    Notif -->|stdout JSON logs| Promtail
    GraphQL -->|stdout JSON logs| Promtail

    App -->|OTLP Traces| OTELCol
    Auth -->|OTLP Traces| OTELCol
    User -->|OTLP Traces| OTELCol
    Notif -->|OTLP Traces| OTELCol

    Promtail -->|Push Log Streams| Loki
    OTELCol -->|Batch & Export Spans| Tempo
    Prometheus -->|Rule Evaluation| Alertmanager

    Grafana -->|Query Metrics| Prometheus
    Grafana -->|Query Logs & TraceID Link| Loki
    Grafana -->|Query Traces| Tempo
```

### Observability Features:
1. **Prometheus RED Metrics:** Tracks Request Rate (`http_requests_total`), Error Rate (`status=~"5.*"`), Latency Histograms (`http_request_duration_seconds`), and custom Kafka Consumer Lag (`kafka_consumer_lag`).
2. **Structured JSON Logs & Loki:** Container stdout logs formatted as JSON with fields `timestamp`, `level`, `service`, `trace_id`, `span_id`, and `message`.
3. **OpenTelemetry & Tempo Tracing:** W3C `traceparent` (`00-{trace_id}-{span_id}-01`) header propagation across HTTP endpoints, gRPC metadata, and Kafka message headers, rendered as distributed waterfall traces in Grafana Tempo.
4. **Grafana Unified UI & Alerting:** Grafana dashboard at `http://localhost:3000` pre-configured with Prometheus, Loki, and Tempo datasources, featuring derived field trace navigation (`Loki log` $\rightarrow$ `Tempo trace`). Alertmanager alerts on `HighErrorRate` (> 1%).

---

## 11. Kubernetes & Helm Cluster Architecture (v0.20)

Phase 20 introduces full Kubernetes orchestration via Helm 3, providing self-healing pod recovery, dynamic horizontal pod autoscaling (HPA), zero-downtime rolling updates, and declarative resource management.

```mermaid
graph TD
    subgraph K8sCluster["Minikube Kubernetes Cluster"]
        IngressController["Nginx Ingress Controller<br/>notenest-ingress"]
        
        subgraph Services["K8s Services (ClusterIP)"]
            SvcGateway["notenest-gateway (Port 8000)"]
            SvcAuth["notenest-auth (Port 50051/9101)"]
            SvcUser["notenest-user (Port 50052/9102)"]
            SvcNote["notenest-note (Port 8080/50053/50054)"]
            SvcGraphQL["notenest-graphql (Port 4000)"]
            SvcNotif["notenest-notification (Port 9103)"]
        end

        subgraph Deployments["K8s Deployments & Autoscaling"]
            DepGateway["Gateway Deployment (Kong)"]
            DepAuth["Auth Deployment"]
            DepUser["User Deployment"]
            DepNote["Note Deployment (2..10 Replicas)"]
            HPANote["HorizontalPodAutoscaler (HPA)<br/>Target CPU: 80%"]
            DepGraphQL["GraphQL Deployment"]
            DepNotif["Notification Deployment"]
        end

        subgraph Config["K8s Configuration & Secrets"]
            ConfigMap["notenest-config ConfigMap"]
            Secret["notenest-secret Secret"]
        end
    end

    IngressController -->|Route / | SvcGateway
    SvcGateway --> DepGateway

    DepGateway --> SvcNote
    DepGateway --> SvcGraphQL

    SvcAuth --> DepAuth
    SvcUser --> DepUser
    SvcNote --> DepNote
    SvcGraphQL --> DepGraphQL
    SvcNotif --> DepNotif

    HPANote -.->|Auto-scale Replicas| DepNote
    ConfigMap -.->|Env Ref| Deployments
    Secret -.->|Secret Ref| Deployments
```

---

## 12. Database Streaming Replication & Read/Write Splitting (v0.21)

NoteNest implements physical WAL streaming replication (1 Primary + 2 Read Replicas) paired with a dual PgBouncer pooler architecture and automatic client fallback.

```mermaid
graph TD
    subgraph Microservices["Microservices Layer"]
        NoteSvc["C++ Note Service<br/>(DBPool / DBConnectionGuard)"]
        AuthSvc["Python Auth Service<br/>(psycopg2 helper)"]
        UserSvc["Python User Service<br/>(psycopg2 helper)"]
        GQLSvc["GraphQL Service<br/>(pg Pool / queryDB)"]
    end

    subgraph Poolers["Dual PgBouncer Poolers (data_net)"]
        PgWrite["PgBouncer Write Pooler<br/>Port 6432"]
        PgRead["PgBouncer Read Pooler<br/>Port 6433"]
    end

    subgraph DatabaseCluster["PostgreSQL Cluster (data_net)"]
        PrimaryDB[("PostgreSQL 15 Primary DB<br/>Port 5432<br/>wal_level = replica")]
        Replica1[("PostgreSQL 15 Read Replica 1<br/>Port 5432<br/>hot_standby = on")]
        Replica2[("PostgreSQL 15 Read Replica 2<br/>Port 5432<br/>hot_standby = on")]
    end

    NoteSvc -->|"Mutations (INSERT/UPDATE/DELETE)"| PgWrite
    NoteSvc -->|"SELECT Queries (Primary Fallback)"| PgRead
    AuthSvc -->|"Signups (INSERT)"| PgWrite
    AuthSvc -->|"Logins (SELECT)"| PgRead
    UserSvc -->|"Profile Lookups (SELECT)"| PgRead
    GQLSvc -->|"Mutations"| PgWrite
    GQLSvc -->|"Queries & DataLoaders"| PgRead

    PgWrite -->|"Transaction Mode (db:5432)"| PrimaryDB
    PgRead -->|"Round-Robin (db-replica-1:5432)"| Replica1
    PgRead -->|"Round-Robin (db-replica-2:5432)"| Replica2

    PrimaryDB ==>|"WAL Physical Streaming Replication"| Replica1
    PrimaryDB ==>|"WAL Physical Streaming Replication"| Replica2
```

### Key Architectural Characteristics
- **WAL Physical Streaming Replication**: Primary Postgres configured with `wal_level = replica`, `max_wal_senders = 10`, `max_replication_slots = 10`, and `hot_standby = on`. Read Replicas are initialized via `pg_basebackup -R` and stream WAL records asynchronously with near-zero latency (<5ms / 0 bytes lag).
- **Dual PgBouncer Connection Pools**: `pgbouncer-write` (port 6432) routes mutation traffic to Primary (`db:5432`). `pgbouncer-read` (port 6433) load-balances read traffic across `db-replica-1:5432` and `db-replica-2:5432` via Docker DNS round-robin (`db-replicas`).
- **Application Read/Write Splitting**: C++ `DBPool` / `DBConnectionGuard` (`DBPoolMode::READ` & `DBPoolMode::WRITE`), Python (`auth`, `user`), and Node.js (`graphql`) route read queries (`SELECT`) to `pgbouncer-read` and mutations (`INSERT`, `UPDATE`, `DELETE`) to `pgbouncer-write`.
- **Automatic Fallback to Primary**: If read replicas or read pool are unavailable or circuit breaker opens, `DBConnectionGuard` and microservice DB helpers catch read errors and automatically fall back to `pgbouncer-write` (Primary DB) to guarantee 100% service uptime with zero dropped requests.
- **Lag Monitoring**: Replicas are monitored via `pg_stat_replication` (`scripts/check_replica_lag.sh`); replicas exceeding 10MB replication lag are marked unready.

---

## 13. Terraform Infrastructure as Code Architecture (v0.22)

Phase 22 introduces declarative Infrastructure as Code (IaC) using Terraform with the `kreuzwerker/docker` provider.

```mermaid
graph TD
    subgraph TerraformRoot["Terraform Root Engine (deployments/terraform)"]
        MainTF["main.tf / variables.tf / outputs.tf"]
        State["State File (terraform.tfstate)"]
        Workspace["Workspaces (dev / stage / e2e)"]
    end

    subgraph Modules["Reusable Infrastructure Modules (deployments/terraform/modules/)"]
        ModPostgres["postgres Module"]
        ModKafka["kafka Module"]
        ModPrometheus["prometheus Module"]
        ModRedis["redis Module"]
        ModMinIO["minio Module"]
        ModConsul["consul Module"]
        ModMicroservices["microservices Module<br/>(auth, user, notification, graphql)"]
        ModAuxiliary["auxiliary Module<br/>(pgbouncer, origin/edge nginx, kong, haproxy, LGTM stack, bastion)"]
        ModApp["app Module<br/>(C++ app & Nginx proxy)"]
    end

    subgraph ProvisionedInfra["Docker Engine Managed Infrastructure"]
        Networks["Docker Networks<br/>(public_net, app_net, data_net, mgmt_net)"]
        Containers["Container Instances<br/>(notenest-${env}-*)"]
    end

    MainTF -->|Reads Workspace Config| Workspace
    MainTF -->|Tracks Provisioned State| State
    MainTF --> ModPostgres
    MainTF --> ModKafka
    MainTF --> ModPrometheus
    MainTF --> ModRedis
    MainTF --> ModMinIO
    MainTF --> ModConsul
    MainTF --> ModMicroservices
    MainTF --> ModAuxiliary
    MainTF --> ModApp

    ModApp -->|"depends_on"| ModAuxiliary
    Modules -->|Provision Networks & Containers| ProvisionedInfra
```

### Key Architectural Characteristics
- **Modular Infrastructure Breakdown**: Split into 9 reusable modules under `deployments/terraform/modules/` (`postgres`, `kafka`, `prometheus`, `redis`, `minio`, `consul`, `microservices`, `auxiliary`, `app`).
- **Workspace Isolation**: Supports dynamic workspace environments (`dev`, `stage`, `e2e-test-ws`) via `terraform workspace`. Resource naming (`notenest-${env}-*`) and subnet allocations auto-scope per workspace to avoid address collisions.
- **Provider Abstraction**: Built on `kreuzwerker/docker` provider to simulate multi-tier cloud infrastructure locally, with clean paths to swap with cloud providers (`hashicorp/aws`, `hashicorp/google`).
- **State Tracking & Lifecycle Validation**: Validated via `tests/suites/22_terraform.sh` covering `init`, `validate`, `plan`, `apply`, live container inspection, state checking, and clean `destroy`.

---

## 14. CI/CD Workflows & Security Automation Pipeline Architecture (v0.23)

Phase 23 introduces GitHub Actions CI/CD workflows, GitHub Container Registry (GHCR) container image publishing, GitOps deployment automation, and automated security scanning.

```mermaid
graph TD
    subgraph Triggers["Git Event Triggers"]
        PR["Pull Request (main, master, dev)"]
        Push["Push to main / master"]
        Tag["Release Tag (v*.*.*)"]
        Cron["Daily Cron Schedule (02:00 UTC)"]
    end

    subgraph CI["Continuous Integration Workflow (.github/workflows/ci.yml)"]
        Lint["Job: Lint & Code Style<br/>(clang-format, black, flake8, npm)"]
        UnitTest["Job: Unit & Service Build Tests<br/>(make -j, py_compile)"]
        HelmTF["Job: Helm & Terraform Validation<br/>(helm lint, tf validate)"]
        DockerE2E["Job: Docker Compose E2E Integration<br/>(docker compose up + tests/e2e_test_docker.sh)"]
    end

    subgraph Security["Security Scanner Workflow (.github/workflows/security.yml)"]
        TrivyFS["Job: Trivy Filesystem Scan<br/>(aquasecurity/trivy-action)"]
        TrivyImg["Job: Trivy Container Image Scan<br/>(CRITICAL/HIGH severity)"]
        SecretScan["Job: Secret & Credential Scan<br/>(Private keys & AWS secret audit)"]
    end

    subgraph CD["Continuous Deployment Workflow (.github/workflows/cd.yml)"]
        GHCRPush["Job: Build & Push GHCR Images<br/>(ghcr.io/kanakbarfa/notenest:sha)"]
        HelmDeploy["Job: Deploy to k3s Cluster<br/>(helm upgrade --install)"]
    end

    PR -->|Triggers| CI
    PR -->|Triggers| Security
    Push -->|Triggers| CI
    Push -->|Triggers| CD
    Push -->|Triggers| Security
    Tag -->|Triggers| CD
    Cron -->|Triggers| Security

    Lint --> DockerE2E
    UnitTest --> DockerE2E
    HelmTF --> DockerE2E

    GHCRPush --> HelmDeploy
```

### Key Architectural Characteristics
- **Multi-Stage CI Pipeline (`ci.yml`)**: Automated code linting (`clang-format`, `black`, `flake8`, `npm build`), binary compilation (`make -j`), infrastructure linting (`helm lint`, `terraform validate`), and ephemeral container E2E integration test execution on pull requests and branch updates.
- **Continuous Deployment & Registry (`cd.yml`)**: Multi-stage image build and push to GitHub Container Registry (`ghcr.io/${{ github.repository_owner }}/notenest`), tagged with Git SHA and `latest`. Automates GitOps release upgrades to k3s cluster using Helm charts.
- **Security & Secret Governance (`security.yml`)**: Continuous vulnerability scanning using Aquasecurity Trivy for filesystem dependencies and built container images, combined with automated repository secret audits to prevent key leakages.
- **Branch Protection & Verification**: Enforces required CI status checks (`lint-code`, `unit-and-integration-tests`, `helm-and-tf-validation`, `docker-e2e-integration`) prior to PR merge. Validated via `tests/suites/23_cicd_workflows.sh`.


