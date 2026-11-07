#!/usr/bin/env python3
"""Development helper: talk to the camera over its telnet root shell.

  tools/cam.py run 'command'          run a shell command, print output, exit with its status
  tools/cam.py push LOCAL REMOTE      upload a file (via nc on port 9001)
  tools/cam.py pull REMOTE LOCAL      download a file (via nc on port 9001)

Camera address: $CAM (default buddy3d, the default hostname). Command timeout: $T seconds (default 60).
"""
import os
import re
import socket
import sys
import time

HOST = os.environ.get("CAM", "buddy3d")
XFER_PORT = 9001
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240


def _strip_iac(sock, data):
    out, i = bytearray(), 0
    while i < len(data):
        b = data[i]
        if b == IAC and i + 1 < len(data):
            c = data[i + 1]
            if c in (DO, DONT, WILL, WONT) and i + 2 < len(data):
                if c == DO:
                    sock.sendall(bytes([IAC, WONT, data[i + 2]]))
                elif c == WILL:
                    sock.sendall(bytes([IAC, DONT, data[i + 2]]))
                i += 3
                continue
            if c == SB:
                j = data.find(bytes([IAC, SE]), i)
                i = j + 2 if j >= 0 else len(data)
                continue
            i += 2
            continue
        out.append(b)
        i += 1
    return bytes(out)


def connect(port, attempts=10):
    """Connect with retries: Wi-Fi to Wi-Fi traffic on some networks blips."""
    for i in range(attempts):
        try:
            return socket.create_connection((HOST, port), timeout=10)
        except OSError:
            if i == attempts - 1:
                raise
            time.sleep(2)


def run(cmd, timeout=None):
    timeout = timeout or float(os.environ.get("T", "60"))
    s = connect(23)
    s.settimeout(0.5)
    buf, t0 = b"", time.time()
    while time.time() - t0 < 2:  # swallow banner/prompt
        try:
            d = s.recv(65536)
            if not d:
                break
            buf += _strip_iac(s, d)
        except socket.timeout:
            if buf:
                break
    # markers are split with '' so the echoed command line never matches
    s.sendall(b"stty -echo 2>/dev/null; PS1=''; echo __ST''ART__\n")
    s.sendall(b"{\n" + cmd.encode() + b"\n} 2>&1; echo __E''ND__$?\n")
    buf, t0 = b"", time.time()
    while time.time() - t0 < timeout:
        try:
            d = s.recv(65536)
            if not d:
                break
            buf += _strip_iac(s, d)
            if re.search(rb"__END__\d+", buf):
                break
        except socket.timeout:
            pass
    s.sendall(b"exit\n")
    s.close()
    text = buf.decode("utf-8", "replace").replace("\r", "")
    if "__START__\n" in text:
        text = text.split("__START__\n", 1)[1]
    m = re.search(r"__END__(\d+)", text)
    if not m:
        sys.stdout.write(text)
        sys.stderr.write("\n[timeout waiting for command]\n")
        return 124
    sys.stdout.write(text[: m.start()])
    return int(m.group(1))


def _start_nc(remote_cmd):
    run(f"setsid sh -c '{remote_cmd}' </dev/null >/dev/null 2>&1 & sleep 1", timeout=10)


def push(local, remote):
    data = open(local, "rb").read()
    _start_nc(f"nc -l -p {XFER_PORT} > {remote}")
    with connect(XFER_PORT) as s:
        s.sendall(data)
    time.sleep(0.5)
    return run(f"chmod +x {remote} 2>/dev/null; ls -l {remote}")


def pull(remote, local):
    _start_nc(f"nc -l -p {XFER_PORT} < {remote}")
    chunks = []
    with connect(XFER_PORT) as s:
        s.settimeout(10)
        while True:
            d = s.recv(65536)
            if not d:
                break
            chunks.append(d)
    open(local, "wb").write(b"".join(chunks))
    print(f"{local}: {sum(map(len, chunks))} bytes")
    return 0


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "run":
        sys.exit(run(sys.argv[2]))
    if len(sys.argv) == 4 and sys.argv[1] == "push":
        sys.exit(push(sys.argv[2], sys.argv[3]))
    if len(sys.argv) == 4 and sys.argv[1] == "pull":
        sys.exit(pull(sys.argv[2], sys.argv[3]))
    sys.stderr.write(__doc__)
    sys.exit(2)
