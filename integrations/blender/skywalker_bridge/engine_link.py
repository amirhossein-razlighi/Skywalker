# SPDX-License-Identifier: GPL-3.0-or-later
# (code that runs inside Blender and uses its Python API follows Blender's license, see docs/LICENSING.md)
"""Talking back to a running Skywalker editor (the "Send to Skywalker" button).

The editor exposes its tools over a Unix domain socket as an MCP server (newline-delimited
JSON-RPC, owner-only permissions). The bridge uses one tool, `dcc_receive`, to hand over a
file Blender just exported.
"""

import json
import os
import socket

DEFAULT_SOCKET = os.path.join(os.path.expanduser("~"), ".skywalker", "editor.sock")


class EngineError(Exception):
    pass


def _rpc(sock, reader, message):
    sock.sendall((json.dumps(message) + "\n").encode("utf-8"))
    if "id" not in message:
        return None
    line = reader.readline()
    if not line:
        raise EngineError("Skywalker closed the connection")
    return json.loads(line.decode("utf-8"))


def call_tool(tool, arguments, socket_path=None, timeout=120.0):
    """Call a Skywalker tool; returns the tool's structured result (a dict)."""
    path = socket_path or DEFAULT_SOCKET
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(timeout)
    try:
        sock.connect(path)
    except OSError as e:
        sock.close()
        raise EngineError("cannot reach the Skywalker editor at %s (%s). Is the editor running with the agent server enabled?" % (path, e))
    try:
        reader = sock.makefile("rb")
        _rpc(sock, reader, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "blender-bridge", "version": "1.0"}}})
        _rpc(sock, reader, {"jsonrpc": "2.0", "method": "notifications/initialized"})
        resp = _rpc(sock, reader, {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                                   "params": {"name": tool, "arguments": arguments}})
    finally:
        sock.close()
    if "error" in resp:
        raise EngineError(resp["error"].get("message", "tool call failed"))
    result = resp.get("result", {})
    if result.get("isError"):
        text = " ".join(b.get("text", "") for b in result.get("content", []))
        raise EngineError(text or "the tool reported an error")
    return result.get("structuredContent") or {}
