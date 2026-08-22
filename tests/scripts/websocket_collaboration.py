#!/usr/bin/env python3
import socket
import sys
import json
import struct
import urllib.parse
import urllib.request
import argparse
import time

class WSConnection:
    def __init__(self, sock, unread_buf=b""):
        self.sock = sock
        self.unread_buf = unread_buf

    def sendall(self, data):
        self.sock.sendall(data)

    def close(self):
        self.sock.close()

    def settimeout(self, timeout):
        self.sock.settimeout(timeout)

    def recv_exact(self, num_bytes):
        data = bytearray()
        if self.unread_buf:
            take = min(num_bytes, len(self.unread_buf))
            data.extend(self.unread_buf[:take])
            self.unread_buf = self.unread_buf[take:]
        while len(data) < num_bytes:
            chunk = self.sock.recv(num_bytes - len(data))
            if not chunk:
                break
            data.extend(chunk)
        return bytes(data)

def fetch_ws_ticket(base_url, token):
    """Exchanges a JWT for a single-use realtime ticket (WS cannot set headers)."""
    req = urllib.request.Request(
        f"{base_url.rstrip('/')}/realtime/ticket",
        method="POST",
        headers={"Authorization": f"Bearer {token}"},
        data=b"",
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode())["ticket"]


def connect_ws(host, port, parsed_path, note_id, token, timeout=10.0):
    base_path = f"{parsed_path}/notes/{note_id}/ws".replace("//", "/")
    scheme = "http" if port not in (443,) else "https"
    base_url = f"{scheme}://{host}:{port}" if port not in (80, 443) else f"{scheme}://{host}"
    ticket = fetch_ws_ticket(base_url + parsed_path, token)
    path = f"{base_path}?ticket={urllib.parse.quote(ticket)}"
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect((host, port))
        host_hdr = host if port in (80, 443) else f"{host}:{port}"
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host_hdr}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        s.sendall(req.encode())
        res = s.recv(4096)
        unread = b""
        if b"\r\n\r\n" in res:
            header_part, payload_part = res.split(b"\r\n\r\n", 1)
            unread = payload_part
            header_str = header_part.decode('utf-8', errors='ignore')
        else:
            header_str = res.decode('utf-8', errors='ignore')

        if "101 Switching Protocols" not in header_str:
            s.close()
            return None
        return WSConnection(s, unread)
    except Exception as e:
        print(f"[connect_ws exception]: {e}")
        try:
            s.close()
        except Exception:
            pass
        return None

def send_frame(ws, payload, opcode=0x01):
    if isinstance(payload, str):
        payload = payload.encode('utf-8')
    length = len(payload)
    mask = b'\x12\x34\x56\x78'
    header = bytearray()
    header.append(0x80 | (opcode & 0x0F))
    if length <= 125:
        header.append(0x80 | length)
    elif length <= 65535:
        header.append(0x80 | 126)
        header.extend(struct.pack('!H', length))
    else:
        header.append(0x80 | 127)
        header.extend(struct.pack('!Q', length))
    header.extend(mask)
    masked_payload = bytearray(b ^ mask[i % 4] for i, b in enumerate(payload))
    ws.sendall(header + masked_payload)

def recv_frame_raw(ws, timeout=5.0):
    ws.settimeout(timeout)
    try:
        header = ws.recv_exact(2)
        if not header or len(header) < 2:
            return None, None
        opcode = header[0] & 0x0F
        masked = (header[1] & 0x80) != 0
        length = header[1] & 0x7F
        if length == 126:
            length = struct.unpack('!H', ws.recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack('!Q', ws.recv_exact(8))[0]
        if masked:
            mask = ws.recv_exact(4)
            data = ws.recv_exact(length)
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        else:
            data = ws.recv_exact(length)
        return opcode, data
    except socket.timeout:
        return None, None

def recv_frame(ws, timeout=5.0):
    op, data = recv_frame_raw(ws, timeout)
    if data is None:
        return None, None
    return op, data.decode('utf-8', errors='ignore')

def main():
    parser = argparse.ArgumentParser(description="WebSocket Collaboration E2E Test")
    parser.add_argument("--server-url", required=True)
    parser.add_argument("--note-id", required=True)
    parser.add_argument("--token-a", required=True)
    parser.add_argument("--token-b", required=True)
    parser.add_argument("--token-c", required=True)
    parser.add_argument("--email-a", required=True)
    parser.add_argument("--email-b", required=True)
    args = parser.parse_args()

    parsed = urllib.parse.urlparse(args.server_url)
    host = parsed.hostname or "127.0.0.1"
    if host == "localhost":
        host = "127.0.0.1"
    port = parsed.port or (80 if parsed.scheme == "http" else 443)

    print("0. Verifying Unauthorized User C handshake rejection...")
    sock_c = connect_ws(host, port, parsed.path, args.note_id, args.token_c, timeout=10.0)
    if sock_c is not None:
        print("Assertion FAILED: Unauthorized User C connected to note room!")
        sock_c.close()
        sys.exit(1)
    print("Unauthorized WebSocket handshake rejection verified.")

    print("1. Connecting User A to WebSocket...")
    sock_a = connect_ws(host, port, parsed.path, args.note_id, args.token_a)
    if sock_a is None:
        print("Assertion FAILED: User A failed to connect WebSocket!")
        sys.exit(1)

    op, msg = recv_frame(sock_a)
    print(f"User A initial frame: {msg}")
    data_a = json.loads(msg)
    assert data_a["type"] == "presence"
    assert data_a["event"] == "user_joined"
    print("User A connected & presence confirmed.")

    print("2. Connecting User B to WebSocket...")
    sock_b = connect_ws(host, port, parsed.path, args.note_id, args.token_b)
    if sock_b is None:
        print("Assertion FAILED: User B failed to connect WebSocket!")
        sys.exit(1)

    op_b, msg_b = recv_frame(sock_b)
    print(f"User B initial frame: {msg_b}")
    assert json.loads(msg_b)["type"] == "presence"

    op_a_join, msg_a_join = recv_frame(sock_a)
    print(f"User A received join presence: {msg_a_join}")
    data_join = json.loads(msg_a_join)
    assert data_join["type"] == "presence"
    assert data_join["event"] == "user_joined"
    print("User B join presence broadcast to User A confirmed.")

    print("3. Testing binary Yjs CRDT frame relay from User A -> User B...")
    bin_payload_a = b"\x00\x00\x01\x02\x03\x04"
    send_frame(sock_a, bin_payload_a, opcode=0x02)
    op_b_bin, data_b_bin = recv_frame_raw(sock_b)
    assert op_b_bin == 0x02, f"Expected opcode 0x02, got {op_b_bin}"
    assert data_b_bin == bin_payload_a, f"Payload mismatch: {data_b_bin} != {bin_payload_a}"
    print("Binary Yjs CRDT frame relay User A -> User B verified.")

    print("4. Testing binary Yjs CRDT frame relay from User B -> User A...")
    bin_payload_b = b"\x00\x01\x05\x06\x07\x08"
    send_frame(sock_b, bin_payload_b, opcode=0x02)
    op_a_bin, data_a_bin = recv_frame_raw(sock_a)
    assert op_a_bin == 0x02, f"Expected opcode 0x02, got {op_a_bin}"
    assert data_a_bin == bin_payload_b, f"Payload mismatch: {data_a_bin} != {bin_payload_b}"
    print("Binary Yjs CRDT frame relay User B -> User A verified.")

    print("5. Testing JSON save event broadcast...")
    send_frame(sock_a, json.dumps({"type": "save", "title": "Test Title", "content": "Test Content"}))
    op_b_save, msg_b_save = recv_frame(sock_b)
    data_b_save = json.loads(msg_b_save)
    assert data_b_save["type"] == "saved"
    op_a_save, msg_a_save = recv_frame(sock_a)
    data_a_save = json.loads(msg_a_save)
    assert data_a_save["type"] == "saved"
    print("JSON save event broadcast verified.")

    print("6. Testing User B disconnect presence update to User A...")
    sock_b.close()

    op_a_left, msg_a_left = recv_frame(sock_a)
    print(f"User A received disconnect frame: {msg_a_left}")
    data_left = json.loads(msg_a_left)
    assert data_left["type"] == "presence"
    assert data_left["event"] == "user_left"
    print("User B disconnect presence update verified.")

    sock_a.close()
    print("All WebSocket collaboration tests passed!")

if __name__ == "__main__":
    main()
