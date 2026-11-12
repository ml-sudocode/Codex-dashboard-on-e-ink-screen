"""Timed hardware alert test through the existing USB sender.

Stop the collector first; leave the USB sender and badge app running.
Writes only the normal local feed and its lock; does not change attention history,
download packages, open USB, flash firmware, or use administrator privileges.
"""
import argparse
import fcntl
import time
import uuid
from pathlib import Path

from collector import atomic_write
from model import revision, stamp, timezone, wire


def test_feed(phase, event, zone, now):
    waiting = bool(event)
    feed = {'mode': 'DEMO', 'timezone': str(zone), 'sampled_at': stamp(now, zone),
            'source': 'OK', 'message': phase,
            'quota': {'status': 'UNAVAILABLE', 'fetched_at': '', 'windows': []},
            'panes': [{'id': 'led-test', 'task': 'Controlled LED test',
                       'status': 'NEEDS_YOU' if waiting else 'WORKING', 'event': event}],
            'footer': {'kind': 'quote', 'text': 'Test data only. No Codex conversations are read.'}}
    feed['revision'] = revision(feed)
    return wire(feed)


def emit_phase(output, zone, message, event, seconds):
    print(message, flush=True)
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        atomic_write(output, test_feed(message, event, zone, time.time()))
        time.sleep(0.5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--state-dir', type=Path, default=Path.home() / '.local/state/codex-board')
    args = parser.parse_args()
    args.state_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    output = args.state_dir / 'feed.txt'
    zone = timezone()
    with (args.state_dir / 'collector.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.error('Stop the collector with Ctrl+C first. Leave the USB sender running.')
        print('40-second test. Each new alert should flash for five seconds.', flush=True)
        try:
            emit_phase(output, zone, 'Watch the LEDs: first alert in 8 seconds.', '', 8)
            emit_phase(output, zone, 'FIRST ALERT sent. Expect one LED sequence, then no repeats.',
                       uuid.uuid4().hex, 12)
            emit_phase(output, zone, 'Request cleared. Second alert in 8 seconds.', '', 8)
            emit_phase(output, zone, 'SECOND ALERT sent. Expect one new LED sequence, then no repeats.',
                       uuid.uuid4().hex, 12)
        except KeyboardInterrupt:
            print('\nTest interrupted.', flush=True)
        finally:
            atomic_write(output, test_feed('Test ended. Restart the live collector.', '', zone, time.time()))
    print('Test ended. Restart the live collector. Report which LED sequences you saw.', flush=True)


if __name__ == '__main__':
    main()
