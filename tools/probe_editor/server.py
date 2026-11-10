"""Local cubemap bounds workbench. Writes only the explicitly selected output."""

import argparse
import json
import secrets
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

from . import document, geometry, storage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bsp", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--runtime-output",
        type=Path,
        help="Optional live adapter .cubemaps.bin destination",
    )
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--no-browser", action="store_true")
    parser.add_argument(
        "--probe-report",
        type=Path,
        help="Explicitly selected native probes.json for the reference fit",
    )
    args = parser.parse_args()
    world = geometry.load(args.bsp)
    output = args.output.resolve()
    if args.runtime_output and args.runtime_output.resolve() == output:
        parser.error("Runtime sidecar must differ from the authoring JSON")
    world["runtimeLive"] = bool(args.runtime_output)
    state = document.empty(world["name"], world["sha256"])
    if args.probe_report:
        reference = document.from_probe_report(
            args.probe_report.read_bytes(), world["name"], world["sha256"]
        )
        state = reference
        world["referenceVolumes"] = reference["volumes"]
    if output.exists():
        state = document.validate(
            json.loads(output.read_text(encoding="utf-8")), world["sha256"]
        )
    token = secrets.token_urlsafe(24)
    assets = Path(__file__).parent
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def respond(self, code, data, content_type="application/json"):
            payload = (
                json.dumps(data).encode()
                if content_type == "application/json"
                else data
            )
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self):
            route = urlparse(self.path).path
            if route in (
                "/",
                "/editor.js",
                "/editor.css",
                "/rotation.mjs",
                "/perspective.mjs",
                "/viewport.mjs",
                "/fly_controls.mjs",
            ):
                name, mime = {
                    "/": ("index.html", "text/html"),
                    "/editor.js": ("editor.js", "text/javascript"),
                    "/editor.css": ("editor.css", "text/css"),
                    "/rotation.mjs": ("rotation.mjs", "text/javascript"),
                    "/perspective.mjs": ("perspective.mjs", "text/javascript"),
                    "/viewport.mjs": ("viewport.mjs", "text/javascript"),
                    "/fly_controls.mjs": ("fly_controls.mjs", "text/javascript"),
                }[route]
                self.respond(200, (assets / name).read_bytes(), mime)
            elif self.headers.get("X-Editor-Token") != token:
                self.respond(403, {"error": "Invalid editor session"})
            elif route == "/scene":
                self.respond(200, world)
            elif route == "/document":
                with lock:
                    self.respond(200, state)
            else:
                self.respond(404, {"error": "Unknown resource"})

        def do_POST(self):
            nonlocal state
            if self.headers.get("X-Editor-Token") != token or self.path != "/document":
                self.respond(403, {"error": "Invalid editor session"})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length <= 0 or length > 4 * 1024 * 1024:
                    raise ValueError("Invalid document size")
                value = document.validate(
                    json.loads(self.rfile.read(length)), world["sha256"]
                )
                with lock:
                    state = storage.save(
                        value, world["sha256"], output, args.runtime_output
                    )
                self.respond(200, {"saved": str(output)})
            except (ValueError, OSError) as error:
                self.respond(400, {"error": str(error)})

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    url = f"http://127.0.0.1:{server.server_port}/#{token}"
    print(f"Cubemap editor: {url}\nOutput: {output}", flush=True)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
