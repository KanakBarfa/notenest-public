#!/bin/bash
set -eo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$SCRIPT_DIR/../common.sh"

echo "[Suite 28] Crash resistance: parser hardening, slowloris guard, recovery..."
register_cleanup

APP_URL="http://localhost:8080"

python3 - <<'PYEOF'
import socket
import sys
import time

HOST, PORT = "127.0.0.1", 8080
fails = []


def raw(payload, read_wait=2.0):
    s = socket.create_connection((HOST, PORT), timeout=5)
    s.sendall(payload)
    time.sleep(read_wait)
    data = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    s.close()
    return data


def check(name, cond, extra=""):
    print(("  PASS " if cond else "  FAIL ") + name + ("" if cond else f"  :: {extra[:120]}"))
    if not cond:
        fails.append(name)


# Smuggling shapes must all be rejected with 400.
r = raw(b"POST /login HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n{}")
check("duplicate Content-Length -> 400", b"400" in r.split(b"\r\n")[0], r[:80])

r = raw(b"POST /login HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 2\r\n\r\n{}")
check("TE+CL smuggling -> 400", b"400" in r.split(b"\r\n")[0], r[:80])

r = raw(b"POST /login HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: identity\r\n\r\n")
check("TE-only framing -> 400", b"400" in r.split(b"\r\n")[0], r[:80])

for bad in [b"abc", b"+5", b"-1", b"99999999999999999999"]:
    r = raw(b"POST /login HTTP/1.1\r\nHost: x\r\nContent-Length: " + bad + b"\r\n\r\n")
    check(f"malformed CL {bad.decode()} -> 400", b"400" in r.split(b"\r\n")[0], r[:60])

r = raw(b"GET /notes HTTP/2.4\r\nHost: x\r\n\r\n")
check("bogus version -> 400", b"400" in r.split(b"\r\n")[0], r[:60])

r = raw(b"BREW /notes HTTP/1.1\r\nHost: x\r\n\r\n")
check("unknown method -> 400", b"400" in r.split(b"\r\n")[0], r[:60])

big = b"A" * 20000
r = raw(b"GET /notes HTTP/1.1\r\nHost: x\r\nX-Big: " + big + b"\r\n\r\n", 3)
check("oversized headers cut off", r == b"" or b"400" in r.split(b"\r\n")[0], repr(r[:60]))

# Slowloris: partial request must be closed by the server within ~30s.
s = socket.create_connection((HOST, PORT), timeout=40)
s.sendall(b"GET /notes HTTP/1.1\r\nHost: x\r\n")  # no final CRLF
t0 = time.time()
s.settimeout(35)
closed = False
try:
    data = s.recv(1024)
    closed = True
except socket.timeout:
    pass
s.close()
elapsed = time.time() - t0
check(f"partial request timed out server-side ({elapsed:.0f}s)", closed and elapsed < 32)

if fails:
    print(f"{len(fails)} crash-resistance checks failed")
    sys.exit(1)
print("--> Parser hardening checks passed.")
PYEOF
py_status=$?
[ "$py_status" -eq 0 ] || exit "$py_status"

# Server must still be fully functional after the abuse burst.
health=$(curl -s -o /dev/null -w "%{http_code}" "$APP_URL/health")
[ "$health" -eq 200 ] || { echo "FAILED: app health degraded after fuzzing (HTTP $health)"; exit 1; }
echo "--> App healthy after abuse burst."

email="crash_$RANDOM@example.com"
assert_request "POST" "/signup" 201 "{\"email\":\"$email\",\"password\":\"Password123!\"}"
token=$(curl -s -X POST "$SERVER_URL/login" -H "Content-Type: application/json" \
    -d "{\"email\":\"$email\",\"password\":\"Password123!\"}" | jq -r '.token')
[ -n "$token" ] && [ "$token" != "null" ] || { echo "FAILED: login after fuzz failed"; exit 1; }
assert_request "POST" "/notes" 201 '{"title":"post-fuzz","content":"ok"}' "$token"
echo "--> Authenticated traffic flows normally after attack."

echo "[Suite 28] PASSED"
