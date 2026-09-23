#!/usr/bin/env python3
"""Serve the built browser target locally.

The engine uses threads, so the page must be cross-origin isolated
(COOP/COEP); a plain `python3 -m http.server` will not work.
"""
import argparse
import http.server

from common import BUILD

OUTPUT = BUILD / 'runtime/platforms/browser'


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(OUTPUT), **kwargs)

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--port', type=int, default=5190)
    args = parser.parse_args()
    if not (OUTPUT / 'melee_browser.wasm').exists():
        raise SystemExit('Nothing to serve: run tools/browser/build.py first.')
    print(f'http://127.0.0.1:{args.port}/')
    http.server.ThreadingHTTPServer(('127.0.0.1', args.port), Handler).serve_forever()


if __name__ == '__main__':
    main()
