"""Serve only the Codex Board feed, from a local file or a fixed Linux VM URL.

Uses Python's standard library. No downloads, file writes, credentials, or badge
flashing. --bind 0.0.0.0 makes the read-only task titles/status/quota reachable
on the local network. Default is localhost only; do not expose it to the Internet.
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import time
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, build_opener

from usb_feed import validate_snapshot


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        return None


def upstream_url(value):
    try:
        parsed = urlsplit(value)
        valid = (parsed.scheme == 'http' and parsed.hostname and parsed.port != 0
                 and not parsed.username and not parsed.password and not parsed.fragment
                 and parsed.path == '/feed.txt' and not parsed.query
                 and not any(c.isspace() for c in value))
    except ValueError:
        valid = False
    if not valid:
        raise argparse.ArgumentTypeError('Use http://VM-IP:8765/feed.txt without credentials or a query')
    return value


def local_feed(path):
    with path.open('rb') as stream:
        if time.time() - os.fstat(stream.fileno()).st_mtime > 30:
            raise ValueError('Collector feed is stale')
        return validate_snapshot(stream.read(8193))


def remote_feed(url):
    # No redirects or inherited proxy settings: the relay reads only its fixed VM URL.
    opener = build_opener(ProxyHandler({}), NoRedirect())
    with opener.open(url, timeout=3) as response:
        if response.status != 200:
            raise ValueError('Upstream feed unavailable')
        return validate_snapshot(response.read(8193))


def make_handler(read_feed):
    class Handler(BaseHTTPRequestHandler):
        server_version = 'CodexBoard'
        sys_version = ''
        timeout = 5

        def log_message(self, format, *args):
            pass  # Do not log arbitrary URL text or repeat every badge poll.

        def reply(self, code, body):
            self.send_response(code)
            self.send_header('Content-Type', 'text/plain; charset=us-ascii')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            if code == 405:
                self.send_header('Allow', 'GET, HEAD')
            self.end_headers()
            if self.command != 'HEAD':
                self.wfile.write(body)

        def do_GET(self):
            if self.path != '/feed.txt':
                self.reply(404, b'Not found\n')
                return
            try:
                body = read_feed()
            except (OSError, ValueError):
                self.reply(503, b'Feed unavailable; check the collector.\n')
                return
            self.reply(200, body)

        do_HEAD = do_GET

        def reject_write(self):
            self.reply(405, b'Read-only feed\n')

        do_POST = do_PUT = do_PATCH = do_DELETE = do_OPTIONS = reject_write

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8765)
    source = parser.add_mutually_exclusive_group()
    source.add_argument('--feed', type=Path)
    source.add_argument('--upstream', type=upstream_url, help='Mac relay: fixed Linux VM feed URL')
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error('Port must be between 1 and 65535')
    path = args.feed or Path.home() / '.local/state/codex-board/feed.txt'
    read_feed = (lambda: remote_feed(args.upstream)) if args.upstream else (lambda: local_feed(path))
    with ThreadingHTTPServer((args.bind, args.port), make_handler(read_feed)) as server:
        print(f'Read-only feed listening on {args.bind}:{args.port}/feed.txt', flush=True)
        print('Source: fixed VM relay' if args.upstream else f'Source: {path}', flush=True)
        print('Only task titles/status/quota are served. Ctrl+C stops the server.', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
