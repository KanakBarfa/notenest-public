#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "[Phase 15] Testing Kafka Event Bus, Outbox Pattern, & Message Queue Architecture..."

USER_EMAIL="kafka_phase15_$RANDOM@example.com"
USER_PASS="Password123!"

echo "1. Registering test user & logging in..."
SIGNUP_RESP=$(curl -s -X POST "$SERVER_URL/signup" \
    -H "Content-Type: application/json" \
    -d "{\"email\":\"$USER_EMAIL\",\"password\":\"$USER_PASS\"}")
LOGIN_RESP=$(curl -s -X POST "$SERVER_URL/login" \
    -H "Content-Type: application/json" \
    -d "{\"email\":\"$USER_EMAIL\",\"password\":\"$USER_PASS\"}")
TOKEN=$(echo "$LOGIN_RESP" | grep -o '"token":"[^"]*' | cut -d'"' -f4 || echo "$LOGIN_RESP" | grep -o '"token": "[^"]*' | cut -d'"' -f4)

if [ -z "$TOKEN" ]; then
    echo "FAILED: User login failed, missing token"
    exit 1
fi

echo "2. Creating a note to trigger Outbox & Kafka pipeline..."
CREATE_RESP=$(curl -s -X POST "$SERVER_URL/notes" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d '{"title":"Kafka Test Note","content":"Testing Kafka Outbox Pattern"}')
NOTE_ID=$(echo "$CREATE_RESP" | grep -o '"id":"[^"]*' | cut -d'"' -f4)

if [ -z "$NOTE_ID" ]; then
    echo "FAILED: Note creation failed"
    exit 1
fi
echo "Created note ID: $NOTE_ID"

sleep 1

if [ -n "$DOCKER_MODE" ]; then
    echo "3. Verifying PostgreSQL Outbox table row status..."
    OUTBOX_CHECK=$(docker exec notenest-db-container psql -U postgres -d postgres -t -c "SELECT published FROM outbox WHERE payload->>'note_id' = '$NOTE_ID';" | tr -d '[:space:]')
    echo "Outbox published state: $OUTBOX_CHECK"
    if [ "$OUTBOX_CHECK" != "t" ]; then
        echo "FAILED: Outbox entry for note $NOTE_ID was not marked published = true"
        exit 1
    fi

    echo "4. Verifying message in Kafka topic 'note.events'..."
    KAFKA_MSG=$(docker exec notenest-kafka-container kafka-console-consumer --bootstrap-server localhost:9092 --topic note.events --from-beginning --max-messages 1 --timeout-ms 5000 2>/dev/null || true)
    echo "Kafka message received: $KAFKA_MSG"
    if ! echo "$KAFKA_MSG" | grep -q "$NOTE_ID"; then
        echo "FAILED: Kafka topic 'note.events' did not receive message for note $NOTE_ID"
        exit 1
    fi

    echo "5. Testing Durability: Stopping Notification Service, creating notes, restarting..."
    docker compose stop notification
    
    CREATE_RESP_2=$(curl -s -X POST "$SERVER_URL/notes" \
        -H "Authorization: Bearer $TOKEN" \
        -H "Content-Type: application/json" \
        -d '{"title":"Offline Note 1","content":"Created while notification consumer offline"}')
    NOTE_ID_2=$(echo "$CREATE_RESP_2" | grep -o '"id":"[^"]*' | cut -d'"' -f4)

    docker compose start notification
    sleep 3

    OUTBOX_CHECK_2=$(docker exec notenest-db-container psql -U postgres -d postgres -t -c "SELECT published FROM outbox WHERE payload->>'note_id' = '$NOTE_ID_2';" | tr -d '[:space:]')
    if [ "$OUTBOX_CHECK_2" != "t" ]; then
        echo "FAILED: Catch-up outbox processing failed after notification service restart"
        exit 1
    fi

    echo "6. Testing Dead Letter Topic (DLT) for malformed poison messages..."
    docker exec notenest-kafka-container bash -c "echo 'INVALID_NON_JSON_POISON_MESSAGE' | kafka-console-producer --bootstrap-server localhost:9092 --topic note.events"
    sleep 2

    DLT_MSG=$(docker exec notenest-kafka-container kafka-console-consumer --bootstrap-server localhost:9092 --topic note.events.DLT --from-beginning --max-messages 1 --timeout-ms 5000 2>/dev/null || true)
    echo "DLT message received: $DLT_MSG"
    if ! echo "$DLT_MSG" | grep -q "INVALID_NON_JSON_POISON_MESSAGE"; then
        echo "FAILED: Poison message was not routed to Dead Letter Topic note.events.DLT"
        exit 1
    fi
fi

echo "7. Testing RabbitMQ PDF Export RPC endpoint (POST /notes/:id/export-pdf)..."
EXPORT_RESP=$(curl -s -X POST "$SERVER_URL/notes/$NOTE_ID/export-pdf" \
    -H "Authorization: Bearer $TOKEN")
echo "PDF Export Response: $EXPORT_RESP"

if ! echo "$EXPORT_RESP" | grep -q '"status"' || ! echo "$EXPORT_RESP" | grep -q '"completed"'; then
    echo "FAILED: PDF Export RPC call did not return status completed"
    exit 1
fi

echo "[Phase 15] Kafka Event Bus, Outbox Pattern, & Message Queue integration tests passed successfully!"
