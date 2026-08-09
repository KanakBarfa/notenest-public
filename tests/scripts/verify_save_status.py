#!/usr/bin/env python3
import socket
import sys
import json
import urllib.request
import urllib.parse
import argparse
import time
from websocket_collaboration import connect_ws, send_frame, recv_frame, recv_frame_raw

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-url", default="http://localhost/api")
    args = parser.parse_args()

    # Signup User A and User B
    email_a = f"test_save_a_{int(time.time())}@example.com"
    email_b = f"test_save_b_{int(time.time())}@example.com"
    pwd = "Password123!"

    req_a = urllib.request.Request(f"{args.server_url}/signup", data=json.dumps({"email": email_a, "password": pwd}).encode(), headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req_a)
    login_a_res = json.loads(urllib.request.urlopen(urllib.request.Request(f"{args.server_url}/login", data=json.dumps({"email": email_a, "password": pwd}).encode(), headers={"Content-Type": "application/json"})).read())
    token_a = login_a_res["token"]

    req_b = urllib.request.Request(f"{args.server_url}/signup", data=json.dumps({"email": email_b, "password": pwd}).encode(), headers={"Content-Type": "application/json"})
    urllib.request.urlopen(req_b)
    login_b_res = json.loads(urllib.request.urlopen(urllib.request.Request(f"{args.server_url}/login", data=json.dumps({"email": email_b, "password": pwd}).encode(), headers={"Content-Type": "application/json"})).read())
    token_b = login_b_res["token"]

    # User A creates note
    create_req = urllib.request.Request(f"{args.server_url}/notes", data=json.dumps({"title": "Sync Test", "content": "Initial text"}).encode(), headers={"Content-Type": "application/json", "Authorization": f"Bearer {token_a}"})
    note = json.loads(urllib.request.urlopen(create_req).read())
    note_id = note["id"]

    # User A shares note with User B as editor
    share_req = urllib.request.Request(f"{args.server_url}/notes/{note_id}/share", data=json.dumps({"target_email": email_b, "permission": "editor"}).encode(), headers={"Content-Type": "application/json", "Authorization": f"Bearer {token_a}"})
    urllib.request.urlopen(share_req)

    parsed = urllib.parse.urlparse(args.server_url)
    host = parsed.hostname or "127.0.0.1"
    if host == "localhost":
        host = "127.0.0.1"
    port = parsed.port or (80 if parsed.scheme == "http" else 443)

    # Connect User A & User B
    sock_a = connect_ws(host, port, parsed.path, note_id, token_a)
    assert sock_a is not None, "User A connect failed"
    recv_frame(sock_a) # presence

    sock_b = connect_ws(host, port, parsed.path, note_id, token_b)
    assert sock_b is not None, "User B connect failed"
    recv_frame(sock_b) # presence
    recv_frame(sock_a) # join presence for B

    print("Step 1: User A sends binary Yjs CRDT update...")
    bin_update = b"\x00\x00\x01\x02\x03\x04"
    send_frame(sock_a, bin_update, opcode=0x02)
    op_b, msg_b_bytes = recv_frame_raw(sock_b)
    assert op_b == 0x02, f"Expected opcode 0x02, got {op_b}"
    assert msg_b_bytes == bin_update
    print("Verified User B receives binary Yjs CRDT update!")

    print("Step 2: User A saves note and sends save event...")
    # HTTP Save
    save_req = urllib.request.Request(f"{args.server_url}/notes/{note_id}", data=json.dumps({"title": "Sync Test", "content": "Unsaved edit by User A"}).encode(), headers={"Content-Type": "application/json", "Authorization": f"Bearer {token_a}"}, method="PUT")
    urllib.request.urlopen(save_req)
    # WS Save broadcast
    send_frame(sock_a, json.dumps({"type": "save", "title": "Sync Test", "content": "Unsaved edit by User A"}))

    op_b_save, msg_b_save = recv_frame(sock_b)
    print("User B received save frame:", msg_b_save)
    data_b_save = json.loads(msg_b_save)
    assert data_b_save["type"] == "saved"
    assert data_b_save["content"] == "Unsaved edit by User A"
    print("Verified User B receives saved broadcast!")

    sock_a.close()
    sock_b.close()
    print("All live unsaved delta and save broadcast tests passed 100%!")

if __name__ == "__main__":
    main()
