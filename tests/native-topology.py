"""Run built Pd external against a local signaling fixture (Python stdlib only)."""
import base64
import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("WebSocket closed")
        data += chunk
    return data


def receive(sock):
    opcode, length = exact(sock, 2)
    assert opcode == 0x81, f"Expected JSON text frame: {opcode}"
    masked, length = length & 128, length & 127
    if length == 126:
        length = struct.unpack("!H", exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack("!Q", exact(sock, 8))[0]
    mask = exact(sock, 4) if masked else None
    payload = exact(sock, length)
    if mask:
        payload = bytes(value ^ mask[i % 4] for i, value in enumerate(payload))
    return json.loads(payload)


def send(sock, message):
    payload = json.dumps(message).encode()
    header = bytes([0x81, len(payload)]) if len(payload) < 126 else b"\x81\x7e" + struct.pack("!H", len(payload))
    sock.sendall(header + payload)


def peer(id, role):
    return {"id": id, "name": id, "topology": "star", "role": role}


def run(role):
    with socket.socket() as listener, tempfile.TemporaryDirectory() as directory:
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(10)
        port = listener.getsockname()[1]
        patch = Path(directory) / "topology.pd"
        patch.write_text(rf"""#N canvas 0 0 600 400 12;
#X obj 20 20 loadbang;
#X msg 20 60 topology invalid \, role invalid \, topology star \, connect ws://127.0.0.1:{port} test native \, role {role} \, connect ws://127.0.0.1:{port} test native \, topology mesh \, role host;
#X obj 20 140 p2p.config topology-test;
#X obj 20 180 print topology-test;
#X connect 0 0 1 0;
#X connect 1 0 2 0;
#X connect 2 0 3 0;
""")
        with tempfile.TemporaryFile(mode="w+") as log:
            process = subprocess.Popen(["pd", "-noprefs", "-nogui", "-stderr", "-noaudio", "-r", "48000",
                                        "-path", str(ROOT / "build"), "-open", str(patch)],
                                       stdout=log, stderr=log)
            try:
                sock, _ = listener.accept()
                with sock:
                    sock.settimeout(10)
                    request = b""
                    while not request.endswith(b"\r\n\r\n"):
                        request += exact(sock, 1)
                    headers = dict(line.split(":", 1) for line in request.decode().split("\r\n")[1:] if ":" in line)
                    key = next(value.strip() for name, value in headers.items() if name.lower() == "sec-websocket-key")
                    accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
                    sock.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
                    join = receive(sock)
                    assert join == {"type": "join", "name": "native", "topology": "star", "role": role}, join
                    send(sock, {"type": "welcome", "id": "a-native"})
                    if role == "client":
                        send(sock, {"type": "existing-peers", "peers": [peer("z-client", "client")]})
                        send(sock, {"type": "peer-joined", "from": "z-client2", "peer": peer("z-client2", "client")})
                        send(sock, {"type": "offer", "from": "z-client", "sdp": "invalid"})
                        sock.settimeout(0.5)
                        try:
                            unexpected = receive(sock)
                            raise AssertionError(f"Client signaled another client: {unexpected}")
                        except socket.timeout:
                            pass
                        sock.settimeout(10)
                        expected = ["z-host", "z-replacement"]
                        for host in expected:
                            send(sock, {"type": "peer-joined", "from": host, "peer": peer(host, "host")})
                            while True:
                                message = receive(sock)
                                assert message.get("to") in expected, message
                                if message["type"] == "offer" and message["to"] == host:
                                    break
                            send(sock, {"type": "peer-left", "from": host})
                    else:
                        expected = [f"z-client-{i}" for i in range(19)]
                        send(sock, {"type": "existing-peers", "peers": [peer(id, "client") for id in expected]})
                        offers = set()
                        while len(offers) < 19:
                            message = receive(sock)
                            assert message.get("to") in expected, message
                            if message["type"] == "offer":
                                offers.add(message["to"])
                    time.sleep(0.2)  # Allow queued Pd outlet events to drain.
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                log.seek(0)
                output = log.read()
            for error in ["Invalid topology", "Invalid role", "Star topology requires role",
                          "Disconnect before changing topology", "Disconnect before changing role"]:
                assert error in output, output
            assert "no method" not in output, output
            assert "No free nodes" not in output, output
            if role == "client":
                assert "peer joined z-client" not in output, output
                assert "peer left z-host" in output, output
                assert "peer joined z-replacement" in output, output
            print(f"PASS native star {role}")


if __name__ == "__main__":
    run("client")
    run("host")
