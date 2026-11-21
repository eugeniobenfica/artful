"""artful-web's pages, served on this machine with the headers that let a page's threads share memory
(Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy): python serve.py [folder] [port], by default the
folder artful-web was built in and 8000. Then http://localhost:8000/artful-web.html."""
import http.server
import os
import sys


class Isolated(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".wasm": "application/wasm", ".js": "text/javascript"}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    folder = sys.argv[1] if len(sys.argv) > 1 else "."
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
    os.chdir(folder)
    print(f"http://localhost:{port}/artful-web.html")
    http.server.ThreadingHTTPServer(("127.0.0.1", port), Isolated).serve_forever()
