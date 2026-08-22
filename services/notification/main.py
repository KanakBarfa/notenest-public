import json
import os
import sys
import time
import threading
import base64
from confluent_kafka import Consumer, Producer, KafkaError
import grpc
import pika
import redis

import signal
from consul_client import ConsulClient
from obs_helper import (
    setup_json_logger,
    init_metrics_server,
    HTTP_REQUESTS_TOTAL,
    HTTP_REQUEST_DURATION,
    KAFKA_CONSUMER_LAG,
    parse_traceparent,
    generate_span_id,
    send_otlp_span,
    format_traceparent,
)

import note_pb2
import note_pb2_grpc

KAFKA_BROKERS = os.getenv("KAFKA_BROKERS", "kafka:9092")
NOTE_SERVICE_URL = os.getenv("NOTE_SERVICE_URL", "app:50054")
REDIS_HOST = os.getenv("REDIS_HOST", "redis")
REDIS_PORT = int(os.getenv("REDIS_PORT", 6379))
RABBITMQ_URL = os.getenv("RABBITMQ_URL", "amqp://guest:guest@rabbitmq:5672/")
DLT_TOPIC = "note.events.DLT"
MAX_DELIVERY_ATTEMPTS = 3
LAG_CHECK_INTERVAL_SEC = 5.0
PDF_WORKER_THREADS = int(os.getenv("PDF_WORKER_THREADS", 4))

logger = setup_json_logger("notification-service")


def compute_consumer_lag(consumer):
    """Sum of (high watermark - position) across assigned partitions."""
    total = 0
    try:
        for tp in consumer.assignment():
            try:
                _, hi = consumer.get_watermark_offsets(tp, timeout=1.0, cached=False)
                positions = consumer.position([tp])
                if not positions:
                    continue
                pos = positions[0].offset
                if pos < 0:
                    continue
                total += max(0, hi - pos)
            except Exception:
                continue
    except Exception:
        return 0
    return total


def send_to_dlt(dlt_producer, key, payload_bytes, error, traceparent):
    if not dlt_producer:
        return False
    try:
        dlt_producer.produce(
            DLT_TOPIC,
            key=key,
            value=payload_bytes,
            headers={
                "error": str(error)[:512],
                "failed_at": str(int(time.time())),
                **({"traceparent": traceparent} if traceparent else {}),
            },
        )
        dlt_producer.flush(5)
        return True
    except Exception as dlt_err:
        logger.error(f"Failed publishing to DLT: {dlt_err}")
        return False


def run_kafka_consumer():
    logger.info(f"Connecting to Redis at {REDIS_HOST}:{REDIS_PORT}...")
    r = redis.Redis(host=REDIS_HOST, port=REDIS_PORT, db=0)

    logger.info(
        f"Connecting to gRPC Note/Attachment Service at {NOTE_SERVICE_URL} via Consul discovery..."
    )
    consul_client = ConsulClient()
    grpc_target = consul_client.resolve_grpc_target(
        "attachment-service", fallback_target=NOTE_SERVICE_URL
    )
    logger.info(f"Resolved gRPC Target: {grpc_target}")
    options = [("grpc.lb_policy_name", "round_robin")]
    channel = grpc.insecure_channel(grpc_target, options=options)
    stub = note_pb2_grpc.NotificationServiceStub(channel)

    kafka_conf = {
        "bootstrap.servers": KAFKA_BROKERS,
        "group.id": "notification-group",
        "auto.offset.reset": "earliest",
        "enable.auto.commit": False,  # offsets advance only after delivery/DLT
    }

    producer_conf = {"bootstrap.servers": KAFKA_BROKERS}
    dlt_producer = None
    try:
        dlt_producer = Producer(producer_conf)
    except Exception as e:
        logger.warning(f"Warning initializing DLT producer: {e}")

    consumer = None
    for attempt in range(15):
        try:
            consumer = Consumer(kafka_conf)
            consumer.subscribe(["note.events"])
            logger.info("Subscribed to Kafka topic 'note.events'")
            break
        except Exception as e:
            logger.warning(f"Waiting for Kafka ({attempt+1}/15): {e}")
            time.sleep(2)

    if not consumer:
        logger.error("Fatal: Could not connect to Kafka.")
        sys.exit(1)

    last_lag_check = 0.0
    while True:
        try:
            msg = consumer.poll(1.0)
            if msg is None:
                tnow = time.time()
                if tnow - last_lag_check >= LAG_CHECK_INTERVAL_SEC:
                    last_lag_check = tnow
                    lag = compute_consumer_lag(consumer)
                    KAFKA_CONSUMER_LAG.labels(topic="note.events", group="notification-group").set(
                        lag
                    )
                continue
            if msg.error():
                if msg.error().code() == KafkaError._PARTITION_EOF:
                    continue
                logger.error(f"Kafka error: {msg.error()}")
                continue

            raw_payload = msg.value().decode("utf-8")

            # Extract headers for tracing
            headers_dict = dict(msg.headers()) if msg.headers() else {}
            tp = headers_dict.get("traceparent")
            if isinstance(tp, bytes):
                tp = tp.decode("utf-8")

            trace_id, parent_id = parse_traceparent(tp)
            span_id = generate_span_id()
            start_time = time.time()
            start_ns = int(start_time * 1e9)

            logger.info(
                f"Received Kafka message: {raw_payload}",
                extra={"trace_id": trace_id, "span_id": span_id},
            )

            try:
                data = json.loads(raw_payload)
                if not tp and "trace_id" in data:
                    trace_id = data["trace_id"]
            except (json.JSONDecodeError, UnicodeDecodeError) as parse_err:
                logger.error(
                    f"Malformed message, sending to DLT: {parse_err}", extra={"trace_id": trace_id}
                )
                send_to_dlt(dlt_producer, msg.key(), msg.value(), parse_err, tp)
                HTTP_REQUESTS_TOTAL.labels(
                    service="notification-service", method="KafkaProcess", status="400"
                ).inc()
                consumer.commit(message=msg, asynchronous=False)
                continue

            event_id = data.get("event_id") or (
                msg.key().decode("utf-8") if msg.key() else str(time.time())
            )
            event_type = data.get("event_type", "note_updated")
            target_user = data.get("shared_with_user_id") or data.get("owner_id", "")

            # Cheap duplicate skip; authoritative mark is written after delivery.
            redis_key = f"notif:processed:{event_id}"
            if r.exists(redis_key):
                logger.info(f"Duplicate event {event_id} skipped.", extra={"trace_id": trace_id})
                consumer.commit(message=msg, asynchronous=False)
                continue

            req = note_pb2.NotifyRequest(
                user_id=target_user, event_type=event_type, payload=raw_payload
            )

            grpc_tp = format_traceparent(trace_id, span_id)
            delivered = False
            last_err = None
            for attempt in range(1, MAX_DELIVERY_ATTEMPTS + 1):
                try:
                    resp = stub.Notify(
                        req, timeout=5, metadata=(("traceparent", grpc_tp), ("trace-id", trace_id))
                    )
                    delivered = True
                    logger.info(
                        f"Notification delivered to {target_user} on attempt {attempt}: {resp.delivered}",
                        extra={"trace_id": trace_id},
                    )
                    break
                except grpc.RpcError as grpc_err:
                    last_err = grpc_err
                    backoff = 0.5 * (2 ** (attempt - 1))
                    logger.warning(
                        f"gRPC Notify attempt {attempt}/{MAX_DELIVERY_ATTEMPTS} failed: {grpc_err}; "
                        f"retrying in {backoff:.1f}s",
                        extra={"trace_id": trace_id},
                    )
                    time.sleep(backoff)

            if delivered:
                # Idempotency recorded only once delivery actually succeeded.
                r.set(redis_key, "1", ex=86400)
                HTTP_REQUESTS_TOTAL.labels(
                    service="notification-service", method="KafkaProcess", status="200"
                ).inc()
            else:
                logger.error(
                    f"gRPC Notify exhausted {MAX_DELIVERY_ATTEMPTS} attempts: {last_err}",
                    extra={"trace_id": trace_id},
                )
                send_to_dlt(dlt_producer, msg.key(), msg.value(), last_err, tp)
                HTTP_REQUESTS_TOTAL.labels(
                    service="notification-service", method="KafkaProcess", status="500"
                ).inc()

            end_time = time.time()
            end_ns = int(end_time * 1e9)
            duration = end_time - start_time
            HTTP_REQUEST_DURATION.labels(
                service="notification-service", method="KafkaProcess"
            ).observe(duration)
            send_otlp_span(
                "notification-service",
                "KafkaProcess",
                trace_id,
                span_id,
                parent_id,
                start_ns,
                end_ns,
            )

            # Offset advances only after delivery or DLT handoff.
            consumer.commit(message=msg, asynchronous=False)

        except Exception as e:
            logger.error(f"Exception in consumer loop: {e}", exc_info=True)
            time.sleep(1)


def generate_pdf_bytes(title: str, content: str, user_id: str, note_id: str) -> bytes:
    header = f"Note Title: {title or 'Untitled Note'}\nNote ID: {note_id}\nUser ID: {user_id}\n\nNote Content:\n{content or 'No content'}"
    lines = header.split("\n")
    stream_ops = []
    for line in lines:
        safe_line = line.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
        stream_ops.append(f"({safe_line}) '")

    stream_data = "BT\n/F1 12 Tf\n50 740 Td\n14 TL\n" + "\n".join(stream_ops) + "\nET\n"

    pdf_str = (
        f"%PDF-1.4\n"
        f"1 0 obj <</Type /Catalog /Pages 2 0 R>> endobj\n"
        f"2 0 obj <</Type /Pages /Kids [3 0 R] /Count 1>> endobj\n"
        f"3 0 obj <</Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources <</Font <</F1 5 0 R>>>> >> endobj\n"
        f"4 0 obj <</Length {len(stream_data)}>> stream\n"
        f"{stream_data}"
        f"endstream\nendobj\n"
        f"5 0 obj <</Type /Font /Subtype /Type1 /BaseFont /Helvetica>> endobj\n"
        f"trailer <</Root 1 0 R>>\n%%EOF"
    )
    return pdf_str.encode("latin1")


def declare_pdf_topology(channel):
    channel.exchange_declare(exchange="pdf.dlx", exchange_type="direct", durable=True)
    channel.queue_declare(queue="pdf.requests.DLQ", durable=True)
    channel.queue_bind(exchange="pdf.dlx", queue="pdf.requests.DLQ", routing_key="pdf.requests")
    channel.queue_declare(
        queue="pdf.requests",
        durable=True,
        arguments={
            "x-dead-letter-exchange": "pdf.dlx",
            "x-dead-letter-routing-key": "pdf.requests",
        },
    )
    channel.queue_declare(queue="pdf.replies", durable=True)


def run_rabbitmq_consumer(worker_id=0):
    logger.info(f"Starting RabbitMQ worker {worker_id} connected to {RABBITMQ_URL}...")
    if RABBITMQ_URL.startswith("amqp://"):
        params = pika.URLParameters(RABBITMQ_URL)
    else:
        params = pika.ConnectionParameters(host=RABBITMQ_URL)

    connection = None
    for attempt in range(15):
        try:
            connection = pika.BlockingConnection(params)
            break
        except Exception as e:
            logger.warning(f"[worker-{worker_id}] Waiting for RabbitMQ ({attempt+1}/15): {e}")
            time.sleep(2)

    if not connection:
        logger.warning(f"[worker-{worker_id}] Could not connect to RabbitMQ.")
        return

    def on_request(ch, method, props, body):
        start_time = time.time()
        start_ns = int(start_time * 1e9)
        correlation_id = props.correlation_id or ""
        reply_to = props.reply_to or "pdf.replies"
        trace_id, parent_id, span_id = None, None, generate_span_id()
        try:
            payload = json.loads(body.decode("utf-8"))
            tp = payload.get("traceparent")
            trace_id, parent_id = parse_traceparent(tp)

            logger.info(
                f"Received PDF export request: {payload}",
                extra={"trace_id": trace_id, "span_id": span_id},
            )
            note_id = payload["note_id"]
            title = payload.get("title", "")
            content = payload.get("content", "")
            user_id = payload.get("user_id", "")
            correlation_id = props.correlation_id or payload.get("correlation_id", "")

            pdf_bytes = generate_pdf_bytes(title, content, user_id, note_id)
            pdf_b64 = base64.b64encode(pdf_bytes).decode("utf-8")

            pdf_summary = (
                f"PDF Summary for note '{title}' (ID: {note_id}) exported for user {user_id}."
            )
            resp_data = {
                "status": "completed",
                "pdf_summary": pdf_summary,
                "pdf_base64": pdf_b64,
                "correlation_id": correlation_id,
            }

            ch.basic_publish(
                exchange="",
                routing_key=reply_to,
                properties=pika.BasicProperties(correlation_id=correlation_id, delivery_mode=2),
                body=json.dumps(resp_data),
            )
            ch.basic_ack(delivery_tag=method.delivery_tag)
            logger.info(
                f"Published PDF export response for correlation_id {correlation_id}",
                extra={"trace_id": trace_id},
            )
            HTTP_REQUESTS_TOTAL.labels(
                service="notification-service", method="RabbitMQExportPDF", status="200"
            ).inc()

            end_time = time.time()
            end_ns = int(end_time * 1e9)
            duration = end_time - start_time
            HTTP_REQUEST_DURATION.labels(
                service="notification-service", method="RabbitMQExportPDF"
            ).observe(duration)
            send_otlp_span(
                "notification-service",
                "RabbitMQExportPDF",
                trace_id,
                span_id,
                parent_id,
                start_ns,
                end_ns,
            )

        except (json.JSONDecodeError, KeyError, ValueError) as err:
            # Bad request: reply with failure and dead-letter (no requeue loop).
            logger.error(f"PDF request rejected: {err}", extra={"trace_id": trace_id})
            _reply_failure(ch, reply_to, correlation_id, err)
            ch.basic_nack(delivery_tag=method.delivery_tag, requeue=False)
            HTTP_REQUESTS_TOTAL.labels(
                service="notification-service", method="RabbitMQExportPDF", status="400"
            ).inc()
        except Exception as err:
            # Transient failure: reply with failure and dead-letter for inspection.
            logger.error(
                f"Error handling PDF request: {err}", extra={"trace_id": trace_id}, exc_info=True
            )
            _reply_failure(ch, reply_to, correlation_id, err)
            ch.basic_nack(delivery_tag=method.delivery_tag, requeue=False)
            HTTP_REQUESTS_TOTAL.labels(
                service="notification-service", method="RabbitMQExportPDF", status="500"
            ).inc()

    while True:
        try:
            channel = connection.channel()
            declare_pdf_topology(channel)
            channel.basic_qos(prefetch_count=4)
            channel.basic_consume(queue="pdf.requests", on_message_callback=on_request)
            logger.info(f"[worker-{worker_id}] Consuming 'pdf.requests'...")
            channel.start_consuming()
        except pika.exceptions.ConnectionClosedByBroker as e:
            logger.warning(
                f"[worker-{worker_id}] Broker closed connection: {e}; reconnecting in 3s"
            )
            time.sleep(3)
            try:
                connection = pika.BlockingConnection(params)
            except Exception as recon_err:
                logger.error(f"[worker-{worker_id}] Reconnect failed: {recon_err}")
                return
        except pika.exceptions.AMQPError as e:
            logger.warning(f"[worker-{worker_id}] AMQP error: {e}; retrying in 3s")
            time.sleep(3)


def _reply_failure(ch, reply_to, correlation_id, err):
    try:
        ch.basic_publish(
            exchange="",
            routing_key=reply_to,
            properties=pika.BasicProperties(correlation_id=correlation_id, delivery_mode=2),
            body=json.dumps(
                {
                    "status": "failed",
                    "error": str(err)[:512],
                    "correlation_id": correlation_id,
                }
            ),
        )
    except Exception as pub_err:
        logger.error(f"Failed publishing error reply: {pub_err}")


def run_rabbitmq_worker():
    threads = []
    for i in range(PDF_WORKER_THREADS):
        t = threading.Thread(target=run_rabbitmq_consumer, args=(i,), daemon=True)
        t.start()
        threads.append(t)
    for t in threads:
        t.join()


if __name__ == "__main__":
    init_metrics_server(9103)
    consul = ConsulClient()
    consul.register_service("notification-service", 50055)
    consul.start_heartbeat(4)

    def shutdown(signum, frame):
        logger.info("Shutting down Notification Service...")
        consul.deregister_all_services()
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    try:
        t_rabbit = threading.Thread(target=run_rabbitmq_worker, daemon=True)
        t_rabbit.start()

        run_kafka_consumer()
    finally:
        consul.deregister_all_services()
