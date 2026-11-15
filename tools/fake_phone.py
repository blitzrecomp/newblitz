"""A pretend phone for tests (src/main/phone_pads.cpp): connects like the controller page and plays a script.

  python3 tools/fake_phone.py [PORT] STEP...
  STEP: "s_<buttons>_<x>_<y>" sends a state (A 32768, B 16384, Z 8192, Start 4096; stick -1..1),
        "wait<seconds>" keeps sending the last state for that long. Prints what the app sends back.
"""
import base64, os, socket, struct, sys, time
host, port = "127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8770
script = sys.argv[2:] or []
s = socket.create_connection((host, port))
key = base64.b64encode(os.urandom(16)).decode()
s.sendall(f"GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
resp = b""
while b"\r\n\r\n" not in resp: resp += s.recv(1024)
print(resp.split(b"\r\n")[0].decode())
early = resp.split(b"\r\n\r\n", 1)[1]  # frames sent right after the handshake (the page version)
def send(msg):
    data = msg.encode(); mask = os.urandom(4)
    s.sendall(bytes([0x81, 0x80 | len(data)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
s.settimeout(0.2)
def drain():
    global early
    try:
        d, early = early + s.recv(256), b""
        i = 0
        while i + 2 <= len(d):
            n = d[i + 1] & 0x7F; print("got", d[i + 2:i + 2 + n].decode()); i += 2 + n
    except socket.timeout: pass
for step in script:
    if step.startswith("wait"): 
        t = float(step[4:]); end = time.time() + t
        while time.time() < end: drain(); send(last) if 'last' in dir() else None; time.sleep(0.1)
    else:
        last = step.replace("_", " "); send(last); drain()
drain(); s.close()
