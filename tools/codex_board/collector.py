"""Collect tmux titles and Codex quota into an atomic, read-only badge snapshot."""
import argparse
import asyncio
import fcntl
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

from model import AttentionTracker, STATES, clean, quota_windows, revision, stamp, timezone, wire
from quota import read_limits


def tmux_rows(session=None, selected=(), all_panes=False):
    if all_panes and selected:
        raise ValueError('All-pane collection cannot be combined with a pane filter')
    fields = "#{pane_id}\t#{window_name}\t#{pane_title}\t#{pane_current_command}"
    command = ["tmux", "list-panes"]
    command += ["-s", "-t", session] if session else ["-a"]
    result = subprocess.run(command + ["-F", fields], capture_output=True,
                            text=True, check=True, timeout=3)
    from model import title_status
    rows = []
    found = set()
    for line in result.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) != 4:
            raise ValueError("Unexpected tmux metadata format")
        pane, window, title, executable = parts
        if not all_panes and pane not in selected and (selected or executable != "codex"):
            continue
        if pane in found:  # Linked tmux windows can appear in multiple sessions.
            continue
        task, status = title_status(title, window)
        found.add(pane)
        rows.append({"id": clean(pane, 32), "task": task, "status": status})
    if set(selected) - found:
        raise ValueError("A selected pane is unavailable")
    if len(rows) > 16:
        raise ValueError("More than sixteen panes; select a smaller session")
    return rows


def report_status_changes(previous, current):
    """Log only pane IDs and interpreted state, never terminal contents."""
    old = {row['id']: row for row in previous}
    for row in current:
        before = old.get(row['id'], {})
        if (row['status'], row['event']) != (before.get('status'), before.get('event')):
            event = ' (LED event)' if row['event'] != before.get('event') and row['event'] else ''
            print(f"Pane {row['id']}: {row['status']}{event}", flush=True)


def finish_notifications(rows, enabled=False):
    """Distinguish a finished reply from a blocking request, without reading text."""
    return [{**row, 'status': 'REPLY_READY' if enabled and row['status'] in ('IDLE', 'DONE')
             else row['status']} for row in rows]


def demo_rows(path):
    raw = json.loads(path.read_text())
    if not isinstance(raw, list) or len(raw) > 16:
        raise ValueError("Demo must contain at most sixteen pane objects")
    rows = []
    for row in raw:
        if row["status"] not in STATES:
            raise ValueError("Unknown demo status")
        rows.append({"id": clean(row["id"], 32), "task": clean(row["task"], 64),
                     "status": row["status"]})
    ids = [row["id"] for row in rows]
    if not all(ids) or len(set(ids)) != len(ids):
        raise ValueError("Demo pane IDs must be nonempty and unique")
    return rows


def atomic_write(path, body):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temporary = tempfile.mkstemp(dir=path.parent, prefix=".feed-")
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(body)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


async def collect(args):
    zone = timezone(args.timezone)
    tracker = AttentionTracker(args.state_dir / ("demo-attention.json" if args.demo else "attention.json"),
                               args.flash_on_ready)
    quota = {"status": "UNAVAILABLE", "fetched_at": "", "windows": []}
    last_rows = []
    last_source_error = None
    started = time.time()

    async def refresh_quota():
        while True:
            try:
                raw = await read_limits()
                windows = quota_windows(raw, zone)
                if not windows:
                    raise ValueError("No quota windows available")
                quota.update(status="OK", fetched_at=stamp(time.time(), zone), windows=windows)
            except (OSError, ValueError, KeyError, RuntimeError, asyncio.TimeoutError):
                quota["status"] = "STALE" if quota["fetched_at"] else "UNAVAILABLE"
                print("Quota unavailable; retaining any previous fetch.", flush=True)
            await asyncio.sleep(300)

    quota_task = None
    if args.demo:
        quota.update(status="OK", fetched_at=stamp(started, zone), windows=[
            {"label": "Demo 5h", "remaining": 28, "resets_at": stamp(started + 3600, zone), "low": True},
            {"label": "Demo weekly", "remaining": 76, "resets_at": stamp(started + 86400, zone), "low": False}])
    else:
        quota_task = asyncio.create_task(refresh_quota())
    try:
        while True:
            now = time.time()
            source = "OK"
            try:
                rows = demo_rows(args.demo) if args.demo else await asyncio.to_thread(
                    tmux_rows, args.session, args.pane, args.all_panes)
                current_rows = tracker.update(finish_notifications(
                    rows, args.mark_ready or args.flash_on_ready))
                report_status_changes(last_rows, current_rows)
                last_rows = current_rows
                last_source_error = None
            except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
                source = "STALE" if last_rows else "UNAVAILABLE"
                kind = type(error).__name__
                if kind != last_source_error:
                    print("Pane source unavailable: " + kind + "; retaining prior rows.", flush=True)
                last_source_error = kind
            footer = {"kind": "quote", "text": "One useful step at a time."}
            try:
                entries = json.loads(args.footer.read_text())
                kind = "joke" if int((now - started) // 60) % 2 else "quote"
                choices = [x for x in entries if x["kind"] == kind]
                entry = choices[int((now - started) // 120) % len(choices)]
                footer = {"kind": kind, "text": clean(entry["text"], 160)}
            except (OSError, ValueError, KeyError, TypeError, ZeroDivisionError):
                pass
            message = clean(args.message_file.read_text(), 120) if args.message_file else ""
            feed = {"mode": "DEMO" if args.demo else "LIVE", "timezone": clean(str(zone), 48),
                    "sampled_at": stamp(now, zone), "source": source, "message": message,
                    "quota": dict(quota), "panes": last_rows, "footer": footer}
            feed["revision"] = revision(feed)
            atomic_write(args.output, wire(feed))
            if args.once:
                break
            await asyncio.sleep(3)
    finally:
        if quota_task:
            quota_task.cancel()
            await asyncio.gather(quota_task, return_exceptions=True)


def main():
    here = Path(__file__).parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", type=Path, help="Editable JSON pane list; no tmux/login needed")
    parser.add_argument("--session", help="Restrict tmux collection to this session")
    parser.add_argument("--pane", action="append", default=[], help="Explicit tmux pane ID, e.g. %%1; repeatable")
    parser.add_argument("--all-panes", action="store_true",
                        help="Include every pane across all sessions (or within --session if given)")
    parser.add_argument("--mark-ready", action="store_true",
                        help="Label IDLE/DONE as REPLY_READY without flashing")
    parser.add_argument("--flash-on-ready", "--notify-on-finish", action="store_true",
                        help="Flash once when a pane newly becomes REPLY_READY")
    parser.add_argument("--timezone", help="IANA zone, default Linux guest zone")
    parser.add_argument("--message-file", type=Path, help="Optional smoke-test text file")
    parser.add_argument("--footer", type=Path, default=here / "footer.json")
    parser.add_argument("--state-dir", type=Path, default=Path.home() / ".local/state/codex-board")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--once", action="store_true", help="Write one snapshot (quota may still be unavailable)")
    args = parser.parse_args()
    if args.all_panes and (args.pane or args.demo):
        parser.error("--all-panes cannot be combined with --pane or --demo")
    args.output = args.output or args.state_dir / "feed.txt"
    args.state_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (args.state_dir / "collector.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            parser.error("A collector is already using this state directory")
        print("Writing feed to " + str(args.output), flush=True)
        if args.flash_on_ready:
            print("Newly finished replies flash red; existing ready panes do not replay.", flush=True)
        elif args.mark_ready:
            print("Finished replies are REPLY_READY without an LED alert.", flush=True)
        try:
            asyncio.run(collect(args))
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
