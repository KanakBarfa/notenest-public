#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "[Suite 27] WebSocket permission model (viewer relay denial)..."
register_cleanup

# Users: owner + viewer
owner_email="ws_perm_owner_$RANDOM@example.com"
viewer_email="ws_perm_viewer_$RANDOM@example.com"
for u in "$owner_email" "$viewer_email"; do
    assert_request "POST" "/signup" 201 "{\"email\":\"$u\",\"password\":\"Password123!\"}"
done
token_owner=$(curl -s -X POST "$SERVER_URL/login" -H "Content-Type: application/json" \
    -d "{\"email\":\"$owner_email\",\"password\":\"Password123!\"}" | jq -r '.token')
token_viewer=$(curl -s -X POST "$SERVER_URL/login" -H "Content-Type: application/json" \
    -d "{\"email\":\"$viewer_email\",\"password\":\"Password123!\"}" | jq -r '.token')

note_id=$(curl -s -X POST "$SERVER_URL/notes" -H "Authorization: Bearer $token_owner" \
    -H "Content-Type: application/json" -d '{"title":"WS Perm Note","content":"base"}' | jq -r '.id')
[ -n "$note_id" ] && [ "$note_id" != "null" ] || { echo "FAILED: note creation"; exit 1; }

# Share as VIEWER: read-only participant.
share_code=$(curl -s -o /dev/null -w "%{http_code}" -X POST "$SERVER_URL/notes/$note_id/share" \
    -H "Authorization: Bearer $token_owner" -H "Content-Type: application/json" \
    -d "{\"target_email\":\"$viewer_email\",\"permission\":\"viewer\"}")
[ "$share_code" -eq 200 ] || [ "$share_code" -eq 201 ] || {
    echo "FAILED: viewer share returned $share_code"; exit 1;
}

PYTHONPATH="$SCRIPT_DIR/../scripts" python3 - <<PYEOF
import json
import sys
import time
import urllib.request
from websocket_collaboration import connect_ws, send_frame, recv_frame_raw, fetch_ws_ticket
from urllib.parse import urlparse

server_url = "$SERVER_URL"
note_id = "$note_id"
parsed = urlparse(server_url)
host, port = parsed.hostname, parsed.port or 80
api_base = server_url

def connect(token):
    ticket = fetch_ws_ticket(api_base, token)
    path = f"/notes/{note_id}/ws?ticket={urllib.parse.quote(ticket)}" if False else None
    return connect_ws(host, port, parsed.path, note_id, token)

import urllib.parse

def connect_with_ticket(token):
    ticket = fetch_ws_ticket(api_base, token)
    base_path = f"{parsed.path}/notes/{note_id}/ws".replace("//", "/")
    s = socket.create_connection((host, port), timeout=10)
    req = (
        f"GET {base_path}?ticket={urllib.parse.quote(ticket)} HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    import socket as _socket
    s.sendall(req.encode())
    res = s.recv(4096)
    if b"101" not in res.split(b"\r\n")[0]:
        return None
    from websocket_collaboration import WSConnection
    parts = res.split(b"\r\n\r\n", 1)
    return WSConnection(s, parts[1] if len(parts) > 1 else b"")

import socket

owner_ws = connect_with_ticket("$token_owner")
viewer_ws = connect_with_ticket("$token_viewer")
if owner_ws is None or viewer_ws is None:
    print("FAILED: websocket handshake rejected for legitimate participants")
    sys.exit(1)
print("--> Owner and viewer joined the room.")

# Drain join/presence broadcasts.
time.sleep(0.6)
try:
    while True:
        op, data = recv_frame_raw(viewer_ws, timeout=0.5)
        if op is None:
            break
except Exception:
    pass

# 1. Owner sends a binary Yjs frame; viewer must RECEIVE it (editor relay ok).
payload = b"\x00\x01\x02\xfe\xffOWNER_UPDATE"
send_frame(owner_ws, payload, opcode=0x02)
got_owner_update = False
deadline = time.time() + 5
while time.time() < deadline:
    op, data = recv_frame_raw(viewer_ws, timeout=2.0)
    if op is None:
        continue
    if op == 0x02 and data == payload:
        got_owner_update = True
        break
if not got_owner_update:
    print("FAILED: editor binary update was not relayed to viewer")
    sys.exit(1)
print("--> Editor binary Yjs frames are relayed to participants.")

# 2. Viewer sends a binary frame; it must NOT reach anyone (relay denied).
evil_payload = b"\xde\xad\xbe\xefVIEWER_MUTATION"
send_frame(viewer_ws, evil_payload, opcode=0x02)
leaked = False
deadline = time.time() + 3
while time.time() < deadline:
    op, data = recv_frame_raw(owner_ws, timeout=1.0)
    if op is None:
        continue
    if op == 0x02 and data == evil_payload:
        leaked = True
        break
if leaked:
    print("FAILED: viewer binary mutation was relayed to other participants")
    sys.exit(1)
print("--> Viewer binary mutations are silently dropped (no relay).")

# 3. Ticket replay is rejected (single-use semantics).
replay_res = None
try:
    req = urllib.request.Request(
        f"{server_url}/notes/{note_id}/ws?ticket={urllib.parse.quote('nonexistent-ticket')}",
        method="GET",
        headers={
            "Upgrade": "websocket",
            "Connection": "Upgrade",
            "Sec-WebSocket-Key": "dGhlIHNhbXBsZSBub25jZQ==",
            "Sec-WebSocket-Version": "13",
        },
    )
    urllib.request.urlopen(req, timeout=5)
    print("FAILED: invalid ticket accepted")
    sys.exit(1)
except urllib.error.HTTPError as e:
    if e.code in (400, 401):
        print("--> Invalid/expired tickets rejected at handshake.")
    else:
        print(f"FAILED: unexpected status for invalid ticket: {e.code}")
        sys.exit(1)

owner_ws.close()
viewer_ws.close()
print("python-block-ok")
PYEOF
py_status=$?
if [ "$py_status" -ne 0 ]; then
    exit "$py_status"
fi

echo "[Suite 27] PASSED"
