import os
import sys
import uuid
import logging
import concurrent.futures
import time
import hmac
import threading
import grpc
from grpc_reflection.v1alpha import reflection
from grpc_health.v1 import health, health_pb2, health_pb2_grpc
import signal
import psycopg2
from psycopg2.pool import ThreadedConnectionPool
from consul_client import ConsulClient
from obs_helper import (
    setup_json_logger,
    init_metrics_server,
    HTTP_REQUESTS_TOTAL,
    HTTP_REQUEST_DURATION,
    parse_traceparent,
    generate_span_id,
    send_otlp_span,
)

sys.path.append(os.path.dirname(os.path.abspath(__file__)))

import user_pb2
import user_pb2_grpc

logger = setup_json_logger("user-service")

# Required on every RPC as `x-internal-secret` metadata (PERMISSION_DENIED otherwise).
INTERNAL_SHARED_SECRET = os.getenv("INTERNAL_SHARED_SECRET")

POOL_MIN = 1
POOL_MAX = 10

_write_pool = None
_read_pool = None
_read_pool_is_fallback = False
_pools_lock = threading.Lock()


def _write_kwargs():
    db_url = os.getenv("DATABASE_URL")
    if db_url:
        return {"dsn": db_url}
    return {
        "host": os.getenv("DB_HOST", "pgbouncer"),
        "port": int(os.getenv("DB_PORT", "6432")),
        "dbname": os.getenv("DB_NAME", "postgres"),
        "user": os.getenv("DB_USER", "postgres"),
        "password": os.getenv("DB_PASSWORD", "pass"),
    }


def _read_kwargs():
    return {
        "host": os.getenv("DB_READ_HOST", "pgbouncer-read"),
        "port": int(os.getenv("DB_READ_PORT", "6433")),
        "dbname": os.getenv("DB_NAME", "postgres"),
        "user": os.getenv("DB_USER", "postgres"),
        "password": os.getenv("DB_PASSWORD", "pass"),
    }


def get_db_connection(mode="write"):
    """Acquire a pooled connection (created lazily, one pool per mode).

    Callers MUST release via put_db_connection() - never close() directly.
    """
    global _read_pool, _read_pool_is_fallback, _write_pool
    with _pools_lock:
        if mode == "read":
            if _read_pool is None:
                try:
                    _read_pool = ThreadedConnectionPool(POOL_MIN, POOL_MAX, **_read_kwargs())
                except Exception as e:
                    logger.warning(
                        f"Read pool init failed ({_read_kwargs()['host']}:{_read_kwargs()['port']}), falling back to write DSN: {e}"
                    )
                    _read_pool = ThreadedConnectionPool(POOL_MIN, POOL_MAX, **_write_kwargs())
                    _read_pool_is_fallback = True
            return _read_pool.getconn()
        if _write_pool is None:
            _write_pool = ThreadedConnectionPool(POOL_MIN, POOL_MAX, **_write_kwargs())
        return _write_pool.getconn()


def put_db_connection(mode, conn):
    """Return a connection to its pool. Discards broken/closed connections."""
    with _pools_lock:
        pool = _read_pool if mode == "read" else _write_pool
    if pool is None:
        try:
            conn.close()
        except Exception:
            pass
        return
    if conn.closed:
        try:
            conn.close()
        except Exception:
            pass
        return
    try:
        pool.putconn(conn)
    except Exception as e:
        logger.error(f"Failed to release DB connection: {e}")
        try:
            conn.close()
        except Exception:
            pass


def check_internal_secret(context):
    """Validate the x-internal-secret metadata on an incoming RPC."""
    if not INTERNAL_SHARED_SECRET:
        logger.error("INTERNAL_SHARED_SECRET not configured; denying internal RPC")
        return False
    metadata = dict(context.invocation_metadata())
    provided = metadata.get("x-internal-secret")
    if not provided:
        return False
    return hmac.compare_digest(str(provided).encode(), INTERNAL_SHARED_SECRET.encode())


def release_read_connection(conn):
    """Roll back any open transaction, then return the connection to its pool."""
    try:
        if conn.status == psycopg2.extensions.STATUS_IN_TRANSACTION:
            conn.rollback()
    except Exception:
        pass
    put_db_connection("read", conn)


def _related_ids_on_cursor(cur, requester_id, target_ids):
    """Subset of target_ids sharing at least one note with requester.

    Relationship covers both directions: requester owns a note shared with the
    target, or the target owns a note shared with the requester.
    """
    sql = """
        SELECT DISTINCT other FROM (
            SELECT s.shared_with_user_id::text AS other
            FROM notes n
            JOIN note_shares s ON s.note_id = n.id
            WHERE n.owner_id::text = %s AND s.shared_with_user_id::text = ANY(%s)
            UNION
            SELECT n.owner_id::text AS other
            FROM notes n
            JOIN note_shares s ON s.note_id = n.id
            WHERE s.shared_with_user_id::text = %s AND n.owner_id::text = ANY(%s)
        ) t
    """
    cur.execute(sql, (requester_id, target_ids, requester_id, target_ids))
    return {row[0] for row in cur.fetchall()}


class TracingInterceptor(grpc.ServerInterceptor):
    def intercept_service(self, continuation, handler_call_details):
        metadata = dict(handler_call_details.invocation_metadata)
        tp = metadata.get("traceparent") or metadata.get("trace-id") or metadata.get("x-trace-id")
        trace_id, parent_id = parse_traceparent(tp)
        span_id = generate_span_id()
        method = handler_call_details.method

        start_time = time.time()
        start_ns = int(start_time * 1e9)

        logger.info(
            f"gRPC call starting: {method}", extra={"trace_id": trace_id, "span_id": span_id}
        )

        response = continuation(handler_call_details)

        end_time = time.time()
        end_ns = int(end_time * 1e9)
        duration = end_time - start_time

        HTTP_REQUESTS_TOTAL.labels(service="user-service", method=method, status="200").inc()
        HTTP_REQUEST_DURATION.labels(service="user-service", method=method).observe(duration)
        send_otlp_span("user-service", method, trace_id, span_id, parent_id, start_ns, end_ns)

        return response


class UserServiceServicer(user_pb2_grpc.UserServiceServicer):
    def _lookup_profiles(self, user_ids, requester_id):
        """Fetch user profiles; raises on DB errors.

        Emails are only populated when the call is fully internal (no acting
        end user) or the acting end user has a note relationship with the
        target (or is the target).
        """
        conn = None
        try:
            conn = get_db_connection("read")
            with conn.cursor() as cur:
                cur.execute(
                    "SELECT id, email, created_at FROM users WHERE id::text = ANY(%s)", (user_ids,)
                )
                rows = cur.fetchall()
                related = None
                if requester_id:
                    related = _related_ids_on_cursor(cur, requester_id, user_ids)
        finally:
            if conn is not None:
                release_read_connection(conn)

        users = []
        for row in rows:
            uid = str(row[0])
            include_email = related is None or uid == requester_id or uid in related
            users.append(
                user_pb2.UserProfileResponse(
                    user_id=uid,
                    email=str(row[1]) if include_email else "",
                    created_at=str(row[2]) if row[2] else "",
                )
            )
        return users

    def GetUserProfile(self, request, context):
        if not check_internal_secret(context):
            context.set_code(grpc.StatusCode.PERMISSION_DENIED)
            context.set_details("Missing or invalid internal shared secret")
            return user_pb2.UserProfileResponse()

        user_id = request.user_id.strip()
        if not user_id:
            context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
            context.set_details("User ID required")
            return user_pb2.UserProfileResponse()

        metadata = dict(context.invocation_metadata())
        requester_id = str(metadata.get("x-requester-id") or "").strip()

        try:
            users = self._lookup_profiles([user_id], requester_id)
        except Exception as e:
            logger.error(f"GetUserProfile error: {e}")
            context.set_code(grpc.StatusCode.INTERNAL)
            context.set_details(str(e))
            return user_pb2.UserProfileResponse()

        if not users:
            context.set_code(grpc.StatusCode.NOT_FOUND)
            context.set_details("User not found")
            return user_pb2.UserProfileResponse()

        return users[0]

    def GetUsersByIDs(self, request, context):
        if not check_internal_secret(context):
            context.set_code(grpc.StatusCode.PERMISSION_DENIED)
            context.set_details("Missing or invalid internal shared secret")
            return user_pb2.UsersBatchResponse()

        user_ids = [uid.strip() for uid in request.user_ids if uid.strip()]
        if not user_ids:
            return user_pb2.UsersBatchResponse()

        metadata = dict(context.invocation_metadata())
        requester_id = str(metadata.get("x-requester-id") or "").strip()

        try:
            users = self._lookup_profiles(user_ids, requester_id)
        except Exception as e:
            logger.error(f"GetUsersByIDs error: {e}")
            context.set_code(grpc.StatusCode.INTERNAL)
            context.set_details(str(e))
            return user_pb2.UsersBatchResponse()

        return user_pb2.UsersBatchResponse(users=users)


def close_db_pools():
    """Close all pooled connections (used at shutdown)."""
    global _write_pool, _read_pool
    with _pools_lock:
        for pool in (_write_pool, _read_pool):
            if pool is not None:
                try:
                    pool.closeall()
                except Exception as e:
                    logger.error(f"Error closing DB pool: {e}")
        _write_pool = None
        _read_pool = None


def serve():
    init_metrics_server(9102)
    if not INTERNAL_SHARED_SECRET:
        logger.warning(
            "INTERNAL_SHARED_SECRET is not set - GetUserProfile/GetUsersByIDs "
            "will be denied for all callers (fail-closed). Configure it in the "
            "caller and this service to enable profile lookups."
        )
    port = os.getenv("PORT", "50052")
    server = grpc.server(
        concurrent.futures.ThreadPoolExecutor(max_workers=10), interceptors=[TracingInterceptor()]
    )
    user_pb2_grpc.add_UserServiceServicer_to_server(UserServiceServicer(), server)

    health_servicer = health.HealthServicer()
    health_pb2_grpc.add_HealthServicer_to_server(health_servicer, server)
    health_servicer.set("notenest.user.UserService", health_pb2.HealthCheckResponse.SERVING)
    health_servicer.set("", health_pb2.HealthCheckResponse.SERVING)

    SERVICE_NAMES = (
        user_pb2.DESCRIPTOR.services_by_name["UserService"].full_name,
        health_pb2.DESCRIPTOR.services_by_name["Health"].full_name,
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
        close_db_pools()
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    try:
        server.wait_for_termination()
    finally:
        consul_client.deregister_all_services()


if __name__ == "__main__":
    serve()
