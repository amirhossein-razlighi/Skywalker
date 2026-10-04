"""Finding and starting a headless engine (``skywalker serve`` or ``skywalker mcp``)."""

from __future__ import annotations

import asyncio
import contextlib
import json
import os
import shutil
import tempfile
from pathlib import Path

from ..errors import EngineConnectionError, EngineNotFoundError
from .protocol import _LINE_LIMIT, JsonRpcConnection


def default_editor_socket() -> str:
    """The socket the editor listens on (``~/.skywalker/editor.sock``)."""
    return str(Path.home() / ".skywalker" / "editor.sock")


def find_binary(explicit: str | os.PathLike[str] | None = None) -> str:
    """The ``skywalker`` CLI: explicit path, $SKYWALKER_BIN, PATH, or a build tree near this package."""
    candidates: list[str] = []
    if explicit:
        candidates.append(os.fspath(explicit))
    if os.environ.get("SKYWALKER_BIN"):
        candidates.append(os.environ["SKYWALKER_BIN"])
    on_path = shutil.which("skywalker")
    if on_path:
        candidates.append(on_path)
    here = Path(__file__).resolve()
    for parent in here.parents:
        for preset in ("release", "headless", "debug"):
            candidates.append(str(parent / "build" / preset / "bin" / "skywalker"))
        if (parent / "CMakePresets.json").exists():
            break
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    raise EngineNotFoundError(
        "no skywalker binary found: build it (cmake --preset release && "
        "cmake --build build/release --target skywalker), put it on PATH, or set SKYWALKER_BIN"
    )


class ServeProcess:
    """A ``skywalker serve`` child process: a headless engine with an agent socket.

    The child exits when this process dies (``--lifeline``: its stdin is our pipe).
    """

    def __init__(self, proc: asyncio.subprocess.Process, socket: str, tmpdir: str | None, info: dict[str, object]):
        self.proc = proc
        self.socket = socket
        self.info = info
        self._tmpdir = tmpdir

    @classmethod
    async def start(
        cls,
        project: str | os.PathLike[str] = ".",
        *,
        scene: str | None = None,
        binary: str | None = None,
        socket: str | None = None,
        timeout: float = 60,
    ) -> ServeProcess:
        exe = find_binary(binary)
        tmpdir = None
        if socket is None:
            # Unix socket paths are short (~104 bytes): keep it in a short temp dir.
            tmpdir = tempfile.mkdtemp(prefix="sky-", dir="/tmp" if os.path.isdir("/tmp") else None)
            socket = os.path.join(tmpdir, "engine.sock")
        args = [exe, "serve", "--project", os.fspath(project), "--socket", socket, "--lifeline", "--quiet"]
        if scene:
            args += ["--scene", scene]
        proc = await asyncio.create_subprocess_exec(
            *args, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE
        )
        assert proc.stdout is not None
        try:
            line = await asyncio.wait_for(proc.stdout.readline(), timeout)
        except asyncio.TimeoutError as e:
            proc.kill()
            raise EngineConnectionError("skywalker serve did not become ready in time") from e
        if not line:
            err = b""
            if proc.stderr is not None:
                err = await proc.stderr.read()
            await proc.wait()
            raise EngineConnectionError(f"skywalker serve exited: {err.decode(errors='replace').strip()}")
        try:
            info = json.loads(line)
        except json.JSONDecodeError as e:
            proc.kill()
            raise EngineConnectionError(f"unexpected output from skywalker serve: {line!r}") from e
        return cls(proc, str(info.get("socket", socket)), tmpdir, info)

    async def stop(self, timeout: float = 10) -> None:
        if self.proc.returncode is None:
            if self.proc.stdin is not None:
                with contextlib.suppress(Exception):
                    self.proc.stdin.close()
            with contextlib.suppress(ProcessLookupError):
                self.proc.terminate()
            try:
                await asyncio.wait_for(self.proc.wait(), timeout)
            except asyncio.TimeoutError:
                with contextlib.suppress(ProcessLookupError):
                    self.proc.kill()
                await self.proc.wait()
        if self._tmpdir:
            shutil.rmtree(self._tmpdir, ignore_errors=True)


async def open_stdio(
    project: str | os.PathLike[str] = ".",
    *,
    scene: str | None = None,
    binary: str | None = None,
    client_name: str = "sky-agents",
) -> JsonRpcConnection:
    """``skywalker mcp --project DIR`` as a child process: one connection, no long polls, no py_* hosting."""
    exe = find_binary(binary)
    args = [exe, "mcp", "--project", os.fspath(project)]
    if scene:
        args += ["--scene", scene]
    proc = await asyncio.create_subprocess_exec(
        *args,
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL,
        limit=_LINE_LIMIT,
    )
    assert proc.stdin is not None and proc.stdout is not None

    async def reap() -> None:
        if proc.returncode is None:
            with contextlib.suppress(ProcessLookupError):
                proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), 10)
            except asyncio.TimeoutError:
                proc.kill()

    conn = JsonRpcConnection(proc.stdout, proc.stdin, client_name, on_close=reap)
    await conn.initialize()
    return conn
