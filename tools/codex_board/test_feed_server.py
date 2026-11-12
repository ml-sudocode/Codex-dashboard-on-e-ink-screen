import argparse
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from alert_test import test_feed
from feed_server import local_feed, make_handler, remote_feed, upstream_url
from model import timezone


class Connection:
    def __init__(self, request):
        self.request = io.BytesIO(request)
        self.response = bytearray()

    def makefile(self, *args):
        return self.request

    def sendall(self, data):
        self.response.extend(data)

    def settimeout(self, timeout):
        pass


class FeedServerTests(unittest.TestCase):
    def setUp(self):
        self.body = test_feed('Wi-Fi test', '', timezone('UTC'), 100)

    def request(self, method='GET', path='/feed.txt', reader=None):
        connection = Connection(f'{method} {path} HTTP/1.0\r\nHost: badge\r\n\r\n'.encode())
        make_handler(reader or (lambda: self.body))(connection, ('127.0.0.1', 1), None)
        return bytes(connection.response).split(b'\r\n\r\n', 1)

    def test_get_and_head_have_length_and_no_cache(self):
        headers, body = self.request()
        self.assertIn(b'200 OK', headers)
        self.assertIn(f'Content-Length: {len(self.body)}'.encode(), headers)
        self.assertIn(b'Cache-Control: no-store', headers)
        self.assertEqual(body, self.body)
        headers, body = self.request('HEAD')
        self.assertIn(b'200 OK', headers)
        self.assertEqual(body, b'')

    def test_only_feed_path_is_readable_and_writes_are_rejected(self):
        def unexpected_read():
            self.fail('Non-feed request must never open a file')
        for path in ('/', '/../../etc/passwd', '/feed.txt?file=secret', '/%66eed.txt'):
            self.assertIn(b'404', self.request(path=path, reader=unexpected_read)[0])
        for method in ('POST', 'PUT', 'PATCH', 'DELETE'):
            self.assertIn(b'405', self.request(method, reader=unexpected_read)[0])

    def test_local_feed_is_bounded_and_rejects_stale_or_invalid_data(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'feed.txt'
            path.write_bytes(self.body)
            self.assertEqual(local_feed(path), self.body)
            os.utime(path, (100, 100))
            with self.assertRaises(ValueError):
                local_feed(path)
            for invalid in (b'not a feed', b'x' * 8193, self.body[:-4]):
                path.write_bytes(invalid)
                with self.assertRaises(ValueError):
                    local_feed(path)

    def test_unavailable_feed_is_503_without_error_details(self):
        def missing():
            raise OSError('/private/path must not appear in HTTP errors')
        headers, body = self.request(reader=missing)
        self.assertIn(b'503', headers)
        self.assertNotIn(b'/private', body)

    def test_relay_url_restrictions_and_bounded_read(self):
        url = 'http://192.0.2.2:8765/feed.txt'
        self.assertEqual(upstream_url(url), url)
        for invalid in ('https://host/feed.txt', 'http://user:secret@host/feed.txt',
                        'http://host/anything', 'file:///feed.txt',
                        'http://host/feed.txt?token=secret', 'http://host:0/feed.txt'):
            with self.assertRaises(argparse.ArgumentTypeError):
                upstream_url(invalid)
        with patch('feed_server.build_opener') as opener:
            response = opener.return_value.open.return_value.__enter__.return_value
            response.status = 200
            response.read.return_value = self.body
            self.assertEqual(remote_feed(url), self.body)
            response.read.assert_called_once_with(8193)
            opener.return_value.open.assert_called_once_with(url, timeout=3)


if __name__ == '__main__':
    unittest.main()
