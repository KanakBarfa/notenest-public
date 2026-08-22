#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "[Suite 29] Outbox delivery guarantees (DR-confirmed publishing, Kafka fanout)..."
register_cleanup

email="outbox_$RANDOM@example.com"
assert_request "POST" "/signup" 201 "{\"email\":\"$email\",\"password\":\"Password123!\"}"
token=$(curl -s -X POST "$SERVER_URL/login" -H "Content-Type: application/json" \
    -d "{\"email\":\"$email\",\"password\":\"Password123!\"}" | jq -r '.token')

# Start a topic dumper BEFORE the write so the new event is captured
# regardless of how much history the topic holds.
KAFKA_DUMP=$(mktemp)
docker exec notenest-kafka-container kafka-console-consumer \
    --bootstrap-server localhost:9092 --topic note.events --from-beginning \
    --timeout-ms 20000 > "$KAFKA_DUMP" 2>/dev/null &
CONSUMER_PID=$!
sleep 2

note_res=$(curl -s -X POST "$SERVER_URL/notes" -H "Authorization: Bearer $token" \
    -H "Content-Type: application/json" -d '{"title":"Outbox Note","content":"delivery"}')
note_id=$(echo "$note_res" | jq -r '.id')
[ -n "$note_id" ] && [ "$note_id" != "null" ] || { echo "FAILED: note creation"; exit 1; }
echo "--> Note created; outbox row expected."

# 1. The outbox row must flip to published only after Kafka confirms delivery.
PUBLISHED=false
for attempt in {1..20}; do
    ROW=$(docker exec notenest-db-container psql -U postgres -d postgres -t -A -c \
        "SELECT published FROM outbox WHERE payload::text LIKE '%$note_id%' ORDER BY created_at DESC LIMIT 1;")
    if [ "$ROW" = "t" ]; then
        PUBLISHED=true
        break
    fi
    sleep 1
done
if $PUBLISHED; then
    echo "--> Outbox row marked published after broker acknowledgement."
else
    echo "FAILED: outbox row not confirmed published within 20s"
    exit 1
fi

# 2. The event must actually exist in the Kafka topic (dump captured it).
wait "$CONSUMER_PID" 2>/dev/null || true
TOPIC_COUNT=$(grep -c "$note_id" "$KAFKA_DUMP" || true)
if [ "${TOPIC_COUNT:-0}" -ge 1 ]; then
    echo "--> Event present in 'note.events' topic."
else
    echo "FAILED: note event never reached the Kafka topic"
    rm -f "$KAFKA_DUMP"
    exit 1
fi

# 3. No poison messages on the DLT for healthy events.
DLT_OUT=$(docker exec notenest-kafka-container kafka-console-consumer \
    --bootstrap-server localhost:9092 --topic note.events.DLT --from-beginning \
    --timeout-ms 5000 2>/dev/null | grep -c "$note_id" || true)
rm -f "$KAFKA_DUMP"
if [ "${DLT_OUT:-0}" -eq 0 ]; then
    echo "--> Healthy event did not land on the DLT."
else
    echo "FAILED: healthy event was dead-lettered"
    exit 1
fi

# 4. Notification consumer delivered the SSE notification end-to-end.
DELIVERED=false
for attempt in {1..15}; do
    LOGS=$(docker logs notenest-notification-container 2>&1 | grep -c "$note_id" || true)
    if [ "${LOGS:-0}" -ge 1 ]; then
        DELIVERED=true
        break
    fi
    sleep 1
done
if $DELIVERED; then
    echo "--> Consumer processed the event end-to-end."
else
    echo "FAILED: notification service never processed the event"
    exit 1
fi

echo "[Suite 29] PASSED"
