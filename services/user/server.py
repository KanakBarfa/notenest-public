import os
import sys
import uuid
import logging
import concurrent.futures
import time
import grpc
from grpc_reflection.v1alpha import reflection
from grpc_health.v1 import health, health_pb2, health_pb2_grpc
import signal
import psycopg2
from consul_client import ConsulClient
from obs_helper import setup_json_logger, init_metrics_server, HTTP_REQUESTS_TOTAL, HTTP_REQUEST_DURATION, parse_traceparent, generate_span_id, send_otlp_span

sys.path.append(os.path.dirname(os.path.abspath(__file__)))

import user_pb2
import user_pb2_grpc

logger = setup_json_logger("user-service")

def get_db_connection(mode="write"):
    if mode == "read":
        read_host = os.getenv("DB_READ_HOST", "pgbouncer-read")
        read_port = int(os.getenv("DB_READ_PORT", "6433"))
        dbname = os.getenv("DB_NAME", "postgres")
        user = os.getenv("DB_USER", "postgres")
        password = os.getenv("DB_PASSWORD", "pass")
        try:
            return psycopg2.connect(host=read_host, port=read_port, dbname=dbname, user=user, password=password)
        except Exception as e:
            logger.warning(f"Read pool connection failed ({read_host}:{read_port}), falling back to write pool: {e}")

    db_url = os.getenv("DATABASE_URL")
    if db_url and mode != "read":
        return psycopg2.connect(db_url)
    
    host = os.getenv("DB_HOST", "pgbouncer")
    port = int(os.getenv("DB_PORT", "6432"))
    dbname = os.getenv("DB_NAME", "postgres")
    user = os.getenv("DB_USER", "postgres")
    password = os.getenv("DB_PASSWORD", "pass")
    return psycopg2.connect(host=host, port=port, dbname=dbname, user=user, password=password)

class TracingInterceptor(grpc.ServerInterceptor):
    def intercept_service(self, continuation, handler_call_details):
        metadata = dict(handler_call_details.invocation_metadata)
        tp = metadata.get("traceparent") or metadata.get("trace-id") or metadata.get("x-trace-id")
        trace_id, parent_id = parse_traceparent(tp)
        span_id = generate_span_id()
        method = handler_call_details.method

        start_time = time.time()
        start_ns = int(start_time * 1e9)
        
        logger.info(f"gRPC call starting: {method}", extra={"trace_id": trace_id, "span_id": span_id})

        response = continuation(handler_call_details)

        end_time = time.time()
        end_ns = int(end_time * 1e9)
        duration = end_time - start_time

        HTTP_REQUESTS_TOTAL.labels(service="user-service", method=method, status="200").inc()
        HTTP_REQUEST_DURATION.labels(service="user-service", method=method).observe(duration)
        send_otlp_span("user-service", method, trace_id, span_id, parent_id, start_ns, end_ns)

        return response

class UserServiceServicer(user_pb2_grpc.UserServiceServicer):
    def GetUserProfile(self, request, context):
        user_id = request.user_id.strip()
        if not user_id:
            context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
            context.set_details("User ID required")
            return user_pb2.UserProfileResponse()

        try:
            conn = get_db_connection("read")
            with conn.cursor() as cur:
                cur.execute("SELECT id, email, created_at FROM users WHERE id::text = %s", (user_id,))
                row = cur.fetchone()
            conn.close()

            if not row:
                context.set_code(grpc.StatusCode.NOT_FOUND)
                context.set_details("User not found")
                return user_pb2.UserProfileResponse()

            return user_pb2.UserProfileResponse(
                user_id=str(row[0]),
                email=str(row[1]),
                created_at=str(row[2]) if row[2] else ""
            )
        except Exception as e:
            logger.error(f"GetUserProfile error: {e}")
            context.set_code(grpc.StatusCode.INTERNAL)
            context.set_details(str(e))
            return user_pb2.UserProfileResponse()

    def GetUsersByIDs(self, request, context):
        user_ids = [uid.strip() for uid in request.user_ids if uid.strip()]
        if not user_ids:
            return user_pb2.UsersBatchResponse()

        try:
            conn = get_db_connection("read")
            with conn.cursor() as cur:
                cur.execute("SELECT id, email, created_at FROM users WHERE id::text = ANY(%s)", (user_ids,))
                rows = cur.fetchall()
            conn.close()

            users = [
                user_pb2.UserProfileResponse(
                    user_id=str(r[0]),
                    email=str(r[1]),
                    created_at=str(r[2]) if r[2] else ""
                )
                for r in rows
            ]
            return user_pb2.UsersBatchResponse(users=users)
        except Exception as e:
            logger.error(f"GetUsersByIDs error: {e}")
            context.set_code(grpc.StatusCode.INTERNAL)
            context.set_details(str(e))
            return user_pb2.UsersBatchResponse()

def serve():
    init_metrics_server(9102)
    port = os.getenv("PORT", "50052")
    server = grpc.server(
        concurrent.futures.ThreadPoolExecutor(max_workers=10),
        interceptors=[TracingInterceptor()]
    )
    user_pb2_grpc.add_UserServiceServicer_to_server(UserServiceServicer(), server)

    health_servicer = health.HealthServicer()
    health_pb2_grpc.add_HealthServicer_to_server(health_servicer, server)
    health_servicer.set("notenest.user.UserService", health_pb2.HealthCheckResponse.SERVING)
    health_servicer.set("", health_pb2.HealthCheckResponse.SERVING)

    SERVICE_NAMES = (
        user_pb2.DESCRIPTOR.services_by_name['UserService'].full_name,
        health_pb2.DESCRIPTOR.services_by_name['Health'].full_name,
        reflection.SERVICE_NAME,
    )
    reflection.enable_server_reflection(SERVICE_NAMES, server)

    server.add_insecure_port(f"[::]:{port}")
    server.start()
    logger.info(f"User gRPC Service started on port {port}")

    consul_client = ConsulClient()
    consul_client.register_service("user-service", int(port))
    consul_client.start_heartbeat(4)

    def shutdown(signum, frame):
        logger.info("Shutting down User gRPC Service (connection draining)...")
        consul_client.deregister_all_services()
        server.stop(3)
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    try:
        server.wait_for_termination()
    finally:
        consul_client.deregister_all_services()

if __name__ == "__main__":
    serve()
