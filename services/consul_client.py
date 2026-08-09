import json
import logging
import os
import socket
import threading
import urllib.request

logging.basicConfig(level=logging.INFO, format="[ConsulClient] %(asctime)s - %(levelname)s - %(message)s")


class ConsulClient:
    def __init__(self, consul_host=None, consul_port=None):
        self.consul_host = consul_host or os.getenv("CONSUL_HOST", "consul")
        self.consul_port = int(consul_port or os.getenv("CONSUL_PORT", 8500))
        self.base_url = f"http://{self.consul_host}:{self.consul_port}"
        self.registered_service_ids = []
        self.registered_services = {}
        self.stop_event = threading.Event()
        self.heartbeat_thread = None

    @staticmethod
    def get_container_address():
        try:
            hostname = socket.gethostname()
            ip = socket.gethostbyname(hostname)
            if ip and not ip.startswith("127."):
                return ip
            return hostname
        except Exception:
            return "127.0.0.1"

    def register_service(self, service_name, port, service_id=None, address=None, ttl_seconds=10):
        if not address:
            address = self.get_container_address()
        if not service_id:
            service_id = f"{service_name}-{address}-{port}"

        url = f"{self.base_url}/v1/agent/service/register"
        payload = {
            "ID": service_id,
            "Name": service_name,
            "Tags": ["grpc", "v0.16"],
            "Address": address,
            "Port": port,
            "Check": {
                "CheckID": f"service:{service_id}",
                "Name": f"{service_name} TTL Check",
                "TTL": f"{ttl_seconds}s",
                "DeregisterCriticalServiceAfter": "30s",
            },
        }

        try:
            req = urllib.request.Request(
                url,
                data=json.dumps(payload).encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="PUT",
            )
            with urllib.request.urlopen(req, timeout=3) as resp:
                if resp.status == 200:
                    logging.info(f"Registered service '{service_name}' (ID: {service_id}) at {address}:{port}")
                    if service_id not in self.registered_service_ids:
                        self.registered_service_ids.append(service_id)
                    self.registered_services[service_id] = {
                        "name": service_name,
                        "port": port,
                        "address": address,
                        "ttl": ttl_seconds,
                    }
                    return True
        except Exception as e:
            logging.warning(f"Failed to register service '{service_name}': {e}")
        return False

    def send_heartbeat(self, service_id):
        url = f"{self.base_url}/v1/agent/check/pass/service:{service_id}"
        try:
            req = urllib.request.Request(url, method="PUT")
            with urllib.request.urlopen(req, timeout=2) as resp:
                return resp.status == 200
        except Exception as e:
            logging.debug(f"Heartbeat failed for {service_id}: {e}")
            return False

    def start_heartbeat(self, interval=4):
        self.stop_heartbeat()
        self.stop_event.clear()

        def _loop():
            while not self.stop_event.is_set():
                for sid in list(self.registered_service_ids):
                    if not self.send_heartbeat(sid):
                        meta = self.registered_services.get(sid)
                        if meta:
                            self.register_service(meta["name"], meta["port"], service_id=sid, address=meta["address"], ttl_seconds=meta["ttl"])
                self.stop_event.wait(interval)

        self.heartbeat_thread = threading.Thread(target=_loop, daemon=True)
        self.heartbeat_thread.start()

    def stop_heartbeat(self):
        self.stop_event.set()
        if self.heartbeat_thread and self.heartbeat_thread.is_alive():
            self.heartbeat_thread.join(timeout=2)

    def deregister_service(self, service_id):
        url = f"{self.base_url}/v1/agent/service/deregister/{service_id}"
        try:
            req = urllib.request.Request(url, method="PUT")
            with urllib.request.urlopen(req, timeout=2) as resp:
                if resp.status == 200:
                    logging.info(f"Deregistered service '{service_id}'")
                    return True
        except Exception as e:
            logging.warning(f"Failed to deregister service '{service_id}': {e}")
        return False

    def deregister_all_services(self):
        self.stop_heartbeat()
        for sid in self.registered_service_ids:
            self.deregister_service(sid)
        self.registered_service_ids.clear()

    def discover_service(self, service_name):
        url = f"{self.base_url}/v1/health/service/{service_name}?passing=true"
        endpoints = []
        try:
            req = urllib.request.Request(url, method="GET")
            with urllib.request.urlopen(req, timeout=3) as resp:
                if resp.status == 200:
                    items = json.loads(resp.read().decode("utf-8"))
                    for item in items:
                        svc = item.get("Service", {})
                        node = item.get("Node", {})
                        addr = svc.get("Address") or node.get("Address")
                        port = svc.get("Port")
                        if addr and port:
                            endpoints.append(f"{addr}:{port}")
        except Exception as e:
            logging.warning(f"Failed to discover service '{service_name}': {e}")
        return endpoints

    def resolve_grpc_target(self, service_name, fallback_target=None):
        endpoints = self.discover_service(service_name)
        if endpoints:
            return ",".join(endpoints)
        return fallback_target or f"{service_name}:50051"
