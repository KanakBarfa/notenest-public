# RUNBOOK-001: High Error Rate & p99 Latency SLO Breach

## Symptoms & Alerts
- Alert: `NoteNestHighErrorRate` (> 1% 5xx responses over 5m window).
- Alert: `NoteNestLatencySLOBreach` (p99 latency > 500ms over 5m window).

## Initial Triage
1. Check Grafana HTTP / gRPC Dashboard to identify which service endpoint is experiencing elevated error rates or latency.
2. Review Prometheus metrics:
   `rate(http_requests_total{status=~"5.."}[5m]) / rate(http_requests_total[5m])`
3. Inspect Loki logs filtered by service and level error:
   `{app="note-service"} |= "error" | json`

## Mitigation Steps
1. High Database Load:
   - Check PgBouncer active pool usage and PostgreSQL locks.
   - Verify read replica health: `SELECT client_addr, state, sync_state FROM pg_stat_replication;`
   - Restart unresponsive read replicas if lag > 10MB.
2. Redis Cache Miss Storm:
   - Check Redis connectivity and memory usage via `redis-cli info stats`.
   - Clear stale lock keys if cache stampede lock deadlocks occur.
3. Pod Resource Starvation:
   - Check Kubernetes HPA scaling status: `kubectl get hpa`.
   - Manually scale deployment replicas if automatic scaling is delayed:
     `kubectl scale deployment/note-service --replicas=10`
