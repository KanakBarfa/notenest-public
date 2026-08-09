import os
import sys
import time
import uuid
import logging
import concurrent.futures
import grpc
from grpc_reflection.v1alpha import reflection
from grpc_health.v1 import health, health_pb2, health_pb2_grpc
import psycopg2
from argon2 import PasswordHasher
from argon2.exceptions import VerifyMismatchError
import signal
import jwt
from consul_client import ConsulClient
from obs_helper import setup_json_logger, init_metrics_server, HTTP_REQUESTS_TOTAL, HTTP_REQUEST_DURATION, parse_traceparent, generate_span_id, send_otlp_span

sys.path.append(os.path.dirname(os.path.abspath(__file__)))

import auth_pb2
import auth_pb2_grpc

logger = setup_json_logger("auth-service")
ph = PasswordHasher()

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

        HTTP_REQUESTS_TOTAL.labels(service="auth-service", method=method, status="200").inc()
        HTTP_REQUEST_DURATION.labels(service="auth-service", method=method).observe(duration)
        send_otlp_span("auth-service", method, trace_id, span_id, parent_id, start_ns, end_ns)

        return response

class AuthServiceServicer(auth_pb2_grpc.AuthServiceServicer):
    def __init__(self):
        self.jwt_secret = os.getenv("JWT_SECRET", "default_super_secure_jwt_secret_key_12345_67890")

    def Signup(self, request, context):
        email = request.email.strip()
        password = request.password.strip()

        if not email or not password:
            context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
            context.set_details("Email and password required")
            return auth_pb2.AuthResponse()

        hashed_pw = ph.hash(password)

        conn = None
        try:
            conn = get_db_connection("write")
            with conn.cursor() as cur:
                cur.execute(
                    "INSERT INTO users (email, password_hash) VALUES (%s, %s) RETURNING id, email",
                    (email, hashed_pw)
                )
                row = cur.fetchone()
                conn.commit()
                user_id = str(row[0])
                user_email = str(row[1])

            token = jwt.encode(
                {"user_id": user_id, "iss": "notenest", "exp": int(time.time()) + 86400},
                self.jwt_secret,
                algorithm="HS256"
            )

            return auth_pb2.AuthResponse(user_id=user_id, email=user_email, token=token)
        except Exception as e:
            logger.error(f"Signup error: {e}")
            context.set_code(grpc.StatusCode.ALREADY_EXISTS)
            context.set_details("User already exists or DB error")
            return auth_pb2.AuthResponse()
        finally:
            if conn:
                try:
                    conn.close()
                except Exception:
                    pass

    def Login(self, request, context):
        email = request.email.strip()
        password = request.password.strip()

        if not email or not password:
            context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
            context.set_details("Email and password required")
            return auth_pb2.AuthResponse()

        conn = None
        try:
            conn = get_db_connection("read")
            with conn.cursor() as cur:
                cur.execute("SELECT id, email, password_hash FROM users WHERE email = %s", (email,))
                row = cur.fetchone()

            if not row:
                context.set_code(grpc.StatusCode.UNAUTHENTICATED)
                context.set_details("Invalid email or password")
                return auth_pb2.AuthResponse()

            user_id, user_email, db_hash = str(row[0]), str(row[1]), str(row[2])

            try:
                ph.verify(db_hash, password)
            except VerifyMismatchError:
                context.set_code(grpc.StatusCode.UNAUTHENTICATED)
                context.set_details("Invalid email or password")
                return auth_pb2.AuthResponse()

            expiry = request.expiry_seconds if request.expiry_seconds > 0 else 86400
            token = jwt.encode(
                {"user_id": user_id, "iss": "notenest", "exp": int(time.time()) + expiry},
                self.jwt_secret,
                algorithm="HS256"
            )

            return auth_pb2.AuthResponse(user_id=user_id, email=user_email, token=token)
        except Exception as e:
            logger.error(f"Login error: {e}")
            context.set_code(grpc.StatusCode.INTERNAL)
            context.set_details(str(e))
            return auth_pb2.AuthResponse()
        finally:
            if conn:
                try:
                    conn.close()
                except Exception:
                    pass

    def VerifyToken(self, request, context):
        token = request.token.strip()
        if not token:
            return auth_pb2.VerifyTokenResponse(valid=False, error="Token empty")

        try:
            payload = jwt.decode(token, self.jwt_secret, algorithms=["HS256"])
            user_id = payload.get("user_id")
            if not user_id:
                return auth_pb2.VerifyTokenResponse(valid=False, error="Invalid payload")
            
            return auth_pb2.VerifyTokenResponse(valid=True, user_id=user_id, email="", error="")
        except jwt.ExpiredSignatureError:
            return auth_pb2.VerifyTokenResponse(valid=False, error="Token expired")
        except Exception as e:
            return auth_pb2.VerifyTokenResponse(valid=False, error=str(e))

def serve():
    init_metrics_server(9101)
    port = os.getenv("PORT", "50051")
    server = grpc.server(
        concurrent.futures.ThreadPoolExecutor(max_workers=10),
        interceptors=[TracingInterceptor()]
    )
    auth_pb2_grpc.add_AuthServiceServicer_to_server(AuthServiceServicer(), server)

    health_servicer = health.HealthServicer()
    health_pb2_grpc.add_HealthServicer_to_server(health_servicer, server)
    health_servicer.set("notenest.auth.AuthService", health_pb2.HealthCheckResponse.SERVING)
    health_servicer.set("", health_pb2.HealthCheckResponse.SERVING)

    SERVICE_NAMES = (
        auth_pb2.DESCRIPTOR.services_by_name['AuthService'].full_name,
        health_pb2.DESCRIPTOR.services_by_name['Health'].full_name,
        reflection.SERVICE_NAME,
    )
    reflection.enable_server_reflection(SERVICE_NAMES, server)

    server.add_insecure_port(f"[::]:{port}")
    server.start()
    logger.info(f"Auth gRPC Service started on port {port}")

    consul_client = ConsulClient()
    consul_client.register_service("auth-service", int(port))
    consul_client.start_heartbeat(4)

    def shutdown(signum, frame):
        logger.info("Shutting down Auth gRPC Service (connection draining)...")
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
