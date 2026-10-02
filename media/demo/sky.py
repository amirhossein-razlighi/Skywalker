"""Tiny MCP client for scripting Skywalker (headless stdio engine or the live editor socket)."""
import json, os, socket, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLI = os.path.join(ROOT, "build", "bin", "skywalker")


class Sky:
    def __init__(self, project=".", scene=None, attach=False, name="director"):
        self.next_id = 0
        if attach:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.connect(os.path.expanduser("~/.skywalker/editor.sock"))
            self.reader = self.sock.makefile("rb")
            self.writer = None
        else:
            args = [CLI, "mcp", "--project", project] + (["--scene", scene] if scene else [])
            self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
            self.reader, self.writer, self.sock = self.proc.stdout, self.proc.stdin, None
        self._rpc("initialize", {"protocolVersion": "2025-11-25", "clientInfo": {"name": name}})

    def _send(self, data: bytes):
        if self.sock:
            self.sock.sendall(data)
        else:
            self.writer.write(data)
            self.writer.flush()

    def _rpc(self, method, params):
        self.next_id += 1
        self._send((json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method, "params": params}) + "\n").encode())
        while True:
            line = self.reader.readline()
            if not line:
                raise RuntimeError("engine closed the connection")
            msg = json.loads(line)
            if msg.get("id") == self.next_id:
                if "error" in msg:
                    raise RuntimeError(msg["error"]["message"])
                return msg["result"]

    def call(self, tool, **args):
        r = self._rpc("tools/call", {"name": tool, "arguments": args})
        if r.get("isError"):
            raise RuntimeError(f"{tool}: {r['content'][0]['text']}")
        return r.get("structuredContent", r["content"][0].get("text") if r["content"] else None)

    def text(self, tool, **args):
        r = self._rpc("tools/call", {"name": tool, "arguments": args})
        return "\n".join(c.get("text", "") for c in r["content"] if c["type"] == "text")

    def batch(self, label, ops):
        return self.call("batch", label=label, operations=[{"tool": t, "args": a} for t, a in ops])

    def close(self):
        if self.sock:
            self.sock.close()
        else:
            self.writer.close()
            self.proc.wait(timeout=10)
