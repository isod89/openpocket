#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""OpenPocket offline installer: serves docs/ (installer, editor, packages) on this computer only, at
http://localhost:8765/ (Web MIDI works on localhost). Chrome or Edge. Ctrl+C stops it.
  tools/serve.py [index.html|editor.html] [firmware-dir]
firmware-dir (for example build/ble) replaces docs/firmware as the place the installer reads the packages from."""
import functools, http.server, os, sys, webbrowser

page = sys.argv[1] if len(sys.argv) > 1 else "index.html"
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, "..", "docs"))
fwdir = os.path.abspath(os.path.join(here, "..", sys.argv[2])) if len(sys.argv) > 2 else None

class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".js": "text/javascript", ".mjs": "text/javascript", ".fwsc": "application/octet-stream"}
    def translate_path(self, path):
        p = super().translate_path(path)
        prefix = os.path.join(root, "firmware")
        if fwdir and (p == prefix or p.startswith(prefix + os.sep)):
            return os.path.join(fwdir, os.path.relpath(p, prefix))
        return p
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

for port in range(8765, 8785):
    try:
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(Handler, directory=root))
        break
    except OSError:
        continue
else:
    sys.exit("Could not open a local port (8765 to 8784).")
url = f"http://localhost:{port}/{page}"
print(f"\n  OpenPocket offline installer: {url}\n  Web editor: http://localhost:{port}/editor.html\n  (Chrome or Edge; Ctrl+C to stop)\n")
webbrowser.open(url)
try:
    srv.serve_forever()
except KeyboardInterrupt:
    pass
