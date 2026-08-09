# RUNBOOK-002: Kafka Consumer Lag & Outbox Backpressure

## Symptoms & Alerts
- Alert: `KafkaConsumerLagHigh` (Consumer lag > 5000 messages on `note.events`).
- Alert: `OutboxTableBackpressure` (Unprocessed rows in `outbox` table > 1000).

## Initial Triage
1. Query Kafka consumer group offset details:
   `kafka-consumer-groups --bootstrap-server kafka:9092 --describe --group notification-consumer-group`
2. Check Outbox relay status on `note-service` logs:
   `kubectl logs -l app=note-service | grep "OutboxRelay"`
3. Inspect PostgreSQL outbox queue size:
   `SELECT count(*) FROM outbox WHERE processed = false;`

## Mitigation Steps
1. Consumer Crash or Hang:
   - Restart notification service consumers:
     `kubectl rollout restart deployment/notification-service`
2. Consumer Bottleneck:
   - Scale consumer replicas up to the partition count of the topic:
     `kubectl scale deployment/notification-service --replicas=4`
3. Poison Pill Message in Kafka Topic:
   - Check Dead Letter Topic (`note.events.DLT`) for failed payload logs.
   - Force offset commit past broken message if unparseable payload is blocking partition consumer loop.
