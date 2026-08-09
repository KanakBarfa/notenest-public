import json
import logging
import os
import random
import sys
import time
import threading
import requests
from prometheus_client import Counter, Histogram, Gauge, start_http_server

# Counter for total HTTP/gRPC requests across services
HTTP_REQUESTS_TOTAL = Counter('http_requests_total', 'Total HTTP and gRPC requests', ['service', 'method', 'status'])

# Histogram for request latency across services
HTTP_REQUEST_DURATION = Histogram('http_request_duration_seconds', 'Request latency in seconds', ['service', 'method'])

# Gauge for Kafka consumer lag
KAFKA_CONSUMER_LAG = Gauge('kafka_consumer_lag', 'Kafka consumer lag count', ['topic', 'group'])

def generate_trace_id():
    """Generates a random 32-character hex trace ID."""
    return f"{random.getrandbits(128):032x}"

def generate_span_id():
    """Generates a random 16-character hex span ID."""
    return f"{random.getrandbits(64):016x}"

def parse_traceparent(traceparent):
    """Parses W3C traceparent header into trace_id and parent_span_id."""
    if not traceparent or not isinstance(traceparent, str):
        return generate_trace_id(), generate_span_id()
    parts = traceparent.split('-')
    if len(parts) >= 3 and len(parts[1]) == 32:
        return parts[1], parts[2]
    return generate_trace_id(), generate_span_id()

def format_traceparent(trace_id, span_id):
    """Formats trace_id and span_id into W3C traceparent format."""
    return f"00-{trace_id}-{span_id}-01"

class JsonFormatter(logging.Formatter):
    """JSON log formatter including service name, trace_id, and span_id."""
    def __init__(self, service_name):
        super().__init__()
        self.service_name = service_name

    def format(self, record):
        trace_id = getattr(record, 'trace_id', '00000000000000000000000000000000')
        span_id = getattr(record, 'span_id', '0000000000000000')
        log_obj = {
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(record.created)),
            "level": record.levelname,
            "service": self.service_name,
            "trace_id": trace_id,
            "span_id": span_id,
            "message": record.getMessage()
        }
        return json.dumps(log_obj)

def setup_json_logger(service_name):
    """Configures root logger to output structured JSON logs."""
    logger = logging.getLogger()
    logger.setLevel(logging.INFO)
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(JsonFormatter(service_name))
    logger.handlers = [handler]
    return logger

def init_metrics_server(port):
    """Starts Prometheus metrics HTTP server on specified port."""
    try:
        start_http_server(port)
        print(f"[Observability] Prometheus metrics server started on port {port}")
    except Exception as e:
        print(f"[Observability] Failed to start metrics server on port {port}: {e}")

def send_otlp_span(service_name, span_name, trace_id, span_id, parent_id, start_time_ns, end_time_ns, status_code=1):
    """Sends OTLP trace payload to OTEL Collector in background thread."""
    def export():
        endpoint = os.getenv("OTEL_EXPORTER_OTLP_ENDPOINT", "http://otel-collector:4318/v1/traces")
        payload = {
            "resourceSpans": [{
                "resource": {
                    "attributes": [{
                        "key": "service.name",
                        "value": {"stringValue": service_name}
                    }]
                },
                "scopeSpans": [{
                    "spans": [{
                        "traceId": trace_id,
                        "spanId": span_id,
                        "parentSpanId": parent_id if parent_id else "",
                        "name": span_name,
                        "kind": 1,
                        "startTimeUnixNano": str(start_time_ns),
                        "endTimeUnixNano": str(end_time_ns),
                        "status": {"code": status_code}
                    }]
                }]
            }]
        }
        try:
            requests.post(endpoint, json=payload, timeout=2)
        except Exception:
            pass

    threading.Thread(target=export, daemon=True).start()
