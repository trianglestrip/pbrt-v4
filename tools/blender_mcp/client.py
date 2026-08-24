"""Tiny JSON-over-TCP client for the blender-mcp addon (port 9876)."""
import json, socket, sys


def call(cmd, timeout=120.0):
    s = socket.create_connection(("localhost", 9876), timeout=5)
    s.settimeout(timeout)
    s.sendall(json.dumps(cmd).encode("utf-8"))
    buf = b""
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        try:
            resp = json.loads(buf.decode("utf-8"))
            s.close()
            return resp
        except (json.JSONDecodeError, UnicodeDecodeError):
            continue
    s.close()
    return {"status": "error", "message": "no response"}


def execute_code(code, timeout=300.0):
    return call({"type": "execute_code", "params": {"code": code}}, timeout)


if __name__ == "__main__":
    r = execute_code(sys.argv[1] if len(sys.argv) > 1 else "import bpy; print(list(bpy.data.objects.keys()))")
    print(json.dumps(r, ensure_ascii=False)[:4000])
