#!/usr/bin/env python3
"""Local browser playground backed by the real Quidra compiler."""
from __future__ import annotations

import argparse
import json
import os
import secrets
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import webbrowser
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent
STATIC = ROOT / "static"
MODES = {"run", "check", "fmt", "ir", "llvm"}
MAX_SOURCE = 256 * 1024
MAX_BODY = 300 * 1024
MAX_OUTPUT = 2 * 1024 * 1024


def discover(explicit: str | None) -> Path:
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit).expanduser())
    if os.environ.get("QUIDRA_BIN"):
        candidates.append(Path(os.environ["QUIDRA_BIN"]).expanduser())
    found = shutil.which("quidra")
    if found:
        candidates.append(Path(found))
    name = "quidra.exe" if os.name == "nt" else "quidra"
    repo = ROOT.parent
    for rel in ("build", "build/Debug", "build/Release"):
        candidates.append(repo / rel / name)
    for path in candidates:
        path = path.resolve()
        if path.is_file():
            return path
    raise FileNotFoundError("build Quidra, put it on PATH, or pass --quidra PATH")


def child_env(work: Path) -> dict[str, str]:
    keys = {
        "PATH", "PATHEXT", "SystemRoot", "WINDIR", "ComSpec",
        "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH",
        "TMP", "TEMP", "TMPDIR", "QUIDRA_CLANGXX", "QUIDRA_CLANG",
        "QUIDRA_LLI", "QUIDRA_RUNTIME_LIBRARY", "QUIDRA_JIT_RUNTIME_LIBRARY",
        "QUIDRA_ORC_RUNTIME", "QUIDRA_COMPILER_RT_BUILTINS",
    }
    env = {k: v for k, v in os.environ.items() if k in keys}
    env["HOME"] = str(work)
    if os.name == "nt":
        env["USERPROFILE"] = str(work)
    return env


def kill_group(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    if os.name != "nt":
        try:
            os.killpg(process.pid, signal.SIGKILL)
            return
        except (ProcessLookupError, PermissionError):
            pass
    process.kill()


class Backend:
    def __init__(self, executable: Path, timeout: float):
        self.executable = executable
        self.timeout = timeout
        self.slots = threading.BoundedSemaphore(2)
        try:
            version = subprocess.run(
                [str(executable), "--version"], capture_output=True, text=True,
                timeout=3, check=False,
            ).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            version = ""
        self.version = version or "Quidra"

    def execute(self, mode: str, source: str) -> dict:
        if mode not in MODES:
            raise ValueError(f"unknown mode: {mode}")
        if len(source.encode()) > MAX_SOURCE:
            raise ValueError(f"source exceeds {MAX_SOURCE} bytes")
        with self.slots, tempfile.TemporaryDirectory(prefix="quidra-playground-") as tmp:
            work = Path(tmp)
            path = work / "main.qui"
            path.write_text(source, encoding="utf-8")
            args = [mode, str(path)]
            if mode == "check":
                args.append("--json")
            kwargs = {"start_new_session": True} if os.name != "nt" else {}
            started = time.monotonic()
            process = subprocess.Popen(
                [str(self.executable), *args], cwd=work, env=child_env(work),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                encoding="utf-8", errors="replace", **kwargs,
            )
            timed_out = False
            try:
                stdout, stderr = process.communicate(timeout=self.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                kill_group(process)
                stdout, stderr = process.communicate()
            elapsed = int((time.monotonic() - started) * 1000)
            if mode == "fmt" and not timed_out and process.returncode == 0:
                stdout = path.read_text(encoding="utf-8")
            if timed_out:
                stderr += ("\n" if stderr else "") + f"Timed out after {self.timeout:g}s."
            return {
                "ok": not timed_out and process.returncode == 0,
                "exit_code": None if timed_out else process.returncode,
                "stdout": stdout[:MAX_OUTPUT],
                "stderr": stderr[:MAX_OUTPUT],
                "elapsed_ms": elapsed,
                "timed_out": timed_out,
            }


class Server(ThreadingHTTPServer):
    daemon_threads = True
    def __init__(self, address: tuple[str, int], backend: Backend):
        super().__init__(address, Handler)
        self.backend = backend
        self.token = secrets.token_urlsafe(24)


class Handler(BaseHTTPRequestHandler):
    server_version = "QuidraPlayground/1"

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"[playground] {fmt % args}")

    def end_headers(self) -> None:
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; style-src 'self'; script-src 'self'; "
            "connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; "
            "base-uri 'none'; form-action 'none'",
        )
        super().end_headers()

    @property
    def app(self) -> Server:
        return self.server  # type: ignore[return-value]

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/api/meta":
            self.json(HTTPStatus.OK, {
                "version": self.app.backend.version,
                "modes": sorted(MODES),
                "max_source_bytes": MAX_SOURCE,
                "timeout_seconds": self.app.backend.timeout,
                "request_token": self.app.token,
                "local_only": True,
            })
            return
        assets = {
            "/": ("index.html", "text/html; charset=utf-8"),
            "/index.html": ("index.html", "text/html; charset=utf-8"),
            "/app.js": ("app.js", "text/javascript; charset=utf-8"),
            "/styles.css": ("styles.css", "text/css; charset=utf-8"),
        }
        item = assets.get(self.path)
        if not item:
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        name, kind = item
        data = (STATIC / name).read_bytes()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self) -> None:  # noqa: N802
        if self.path != "/api/execute":
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        if self.headers.get("X-Quidra-Playground-Token") != self.app.token:
            self.json(HTTPStatus.FORBIDDEN, {"error": "invalid request token"})
            return
        if self.headers.get_content_type() != "application/json":
            self.json(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, {"error": "expected application/json"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length < 1 or length > MAX_BODY:
                raise ValueError(f"request must be 1..{MAX_BODY} bytes")
            body = json.loads(self.rfile.read(length))
            mode, source = body.get("mode"), body.get("source")
            if not isinstance(mode, str) or not isinstance(source, str):
                raise ValueError("mode and source must be strings")
            result = self.app.backend.execute(mode, source)
        except (ValueError, json.JSONDecodeError, UnicodeDecodeError) as exc:
            self.json(HTTPStatus.BAD_REQUEST, {"error": str(exc)})
            return
        except OSError as exc:
            self.json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": str(exc)})
            return
        self.json(HTTPStatus.OK, result)

    def json(self, status: HTTPStatus, value: dict) -> None:
        data = (json.dumps(value, ensure_ascii=False) + "\n").encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the local Quidra Playground")
    parser.add_argument("--quidra")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--no-open", action="store_true")
    args = parser.parse_args()
    if not 0 <= args.port <= 65535 or not 0 < args.timeout <= 60:
        parser.error("port must be 0..65535 and timeout must be 0..60 seconds")
    try:
        executable = discover(args.quidra)
    except FileNotFoundError as exc:
        print(f"playground: {exc}", file=sys.stderr)
        return 2
    backend = Backend(executable, args.timeout)
    server = Server(("127.0.0.1", args.port), backend)
    host, port = server.server_address[:2]
    url = f"http://{host}:{port}/"
    print(f"Quidra Playground: {url}")
    print(f"Compiler: {backend.version}")
    if not args.no_open:
        threading.Timer(0.25, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever(0.2)
    except KeyboardInterrupt:
        print("\nStopping Quidra Playground.")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
