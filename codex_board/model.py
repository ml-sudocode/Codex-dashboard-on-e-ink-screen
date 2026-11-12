"""Whitelisted feed fields, local time formatting and attention identities."""
import hashlib
import json
import math
import os
import re
import tempfile
import unicodedata
import uuid
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

STATES = {"WORKING", "IDLE", "DONE", "NEEDS_YOU", "APPROVAL", "UNKNOWN", "REPLY_READY"}
ATTENTION = {"NEEDS_YOU", "APPROVAL"}
STATUS_WORDS = {
    "action required": "NEEDS_YOU", "needs you": "NEEDS_YOU",
    "input required": "NEEDS_YOU", "needs input": "NEEDS_YOU",
    "waiting for input": "NEEDS_YOU", "approval required": "APPROVAL",
    "waiting for approval": "APPROVAL", "working": "WORKING",
    "thinking": "WORKING", "running": "WORKING", "idle": "IDLE",
    "done": "DONE", "completed": "DONE", "ready": "IDLE",
}
STATUS_RE = "|".join(sorted(STATUS_WORDS, key=len, reverse=True))


def clean(value, limit):
    value = re.sub(r"\x1b\][^\x07]*(?:\x07|\x1b\\)", "", str(value))
    value = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", value)
    value = unicodedata.normalize("NFKD", value).encode("ascii", "ignore").decode()
    return " ".join("".join(c if 32 <= ord(c) < 127 else " " for c in value).split())[:limit]


def title_status(title, fallback):
    # Parse status only at a title boundary, never a word inside a task name.
    title = re.sub(r"[\u2800-\u28ff\u25d0-\u25d3]", "", title).strip()
    title = re.sub(r"^codex\s*[:|\-]\s*", "", title, flags=re.I)
    # Codex titles may include a project after the status, e.g.
    # "Task | Action Required | project". Match whole fields, never task prose.
    fields = [field.strip() for field in title.split('|')]
    if len(fields) > 1:
        matches = [(i, STATUS_WORDS[field.lower()]) for i, field in enumerate(fields)
                   if field.lower() in STATUS_WORDS]
        if len(matches) == 1:
            index, status = matches[0]
            task = ' | '.join(field for i, field in enumerate(fields) if i != index and field)
            return clean(task or fallback, 64), status
        if len(matches) > 1:
            return clean(title or fallback, 64), 'UNKNOWN'
    patterns = [rf"^(.*?)\s*\[({STATUS_RE})\]\s*$",
                rf"^(.*?)\s+(?:[|\-–—·•])\s*({STATUS_RE})\s*$"]
    for pattern in patterns:
        match = re.match(pattern, title, flags=re.I)
        if match:
            return clean(match[1] or fallback, 64), STATUS_WORDS[match[2].lower()]
    match = re.match(rf"^({STATUS_RE})\s*[:|\-–—·•]\s*(.+)$", title, flags=re.I)
    if match:
        return clean(match[2], 64), STATUS_WORDS[match[1].lower()]
    if title.lower() in STATUS_WORDS:
        return clean(fallback, 64), STATUS_WORDS[title.lower()]
    return clean(title or fallback, 64), "UNKNOWN"


def timezone(name=None):
    if name:
        return ZoneInfo(name)
    candidates = [os.environ.get("TZ", "").lstrip(":")]
    try:
        candidates.append(Path("/etc/timezone").read_text().strip())
    except OSError:
        pass
    try:
        candidates.append(str(Path("/etc/localtime").resolve()).split("/zoneinfo/", 1)[1])
    except (OSError, IndexError):
        pass
    for candidate in candidates:
        try:
            if candidate:
                return ZoneInfo(candidate)
        except (KeyError, ValueError):
            pass
    with open("/etc/localtime", "rb") as source:
        return ZoneInfo.from_file(source, key="guest-local")


def stamp(epoch, zone):
    return datetime.fromtimestamp(epoch, zone).strftime("%Y-%m-%d %H:%M:%S %Z %z")


def quota_windows(raw, zone):
    buckets = raw.get("rateLimitsByLimitId")
    if not isinstance(buckets, dict) or not buckets:
        single = raw.get("rateLimits")
        buckets = {"codex": single} if isinstance(single, dict) else {}
    windows = []
    for key, bucket in sorted(buckets.items()):
        if not isinstance(bucket, dict):
            continue
        for kind in ("primary", "secondary"):
            window = bucket.get(kind)
            if not isinstance(window, dict):
                continue
            used, reset = window.get("usedPercent"), window.get("resetsAt")
            if not isinstance(used, (float, int)) or not math.isfinite(used):
                continue
            if not isinstance(reset, (float, int)) or not math.isfinite(reset):
                continue
            remaining = max(0, min(100, math.floor(100 - used)))
            duration = window.get("windowDurationMins")
            duration_name = ('5-hour' if duration == 300 else 'Weekly' if duration == 10080
                             else f'{duration} min' if duration else kind)
            prefix = bucket.get('limitName') or (key if key != 'codex' else '')
            label = f'{prefix} {duration_name}'.strip()
            windows.append({"label": clean(label, 32), "remaining": remaining,
                            "resets_at": stamp(reset, zone), "low": remaining < 30})
    if len(windows) > 8:
        raise ValueError("More than eight quota windows; refusing a partial snapshot")
    return windows


class AttentionTracker:
    def __init__(self, path, flash_on_ready=False):
        self.path = Path(path)
        self.flash_on_ready = flash_on_ready
        try:
            value = json.loads(self.path.read_text())
            self.previous = value if isinstance(value, dict) else {}
        except (OSError, ValueError):
            self.previous = {}

    def update(self, rows):
        current = {}
        result = []
        for row in rows:
            row = dict(row)
            old = self.previous.get(row["id"], {})
            if row["status"] in ATTENTION:
                row["event"] = (old.get("event") if old.get("status") in ATTENTION else None) or uuid.uuid4().hex
            elif self.flash_on_ready and row["status"] == "REPLY_READY":
                # A reply already ready at startup is not a new notification.
                if old.get("status") == "REPLY_READY":
                    row["event"] = old.get("event", "")
                else:
                    row["event"] = uuid.uuid4().hex if old else ""
            else:
                row["event"] = ""
            current[row["id"]] = {"status": row["status"], "event": row["event"]}
            result.append(row)
        if current != self.previous:
            self.path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            fd, temporary = tempfile.mkstemp(dir=self.path.parent, prefix=".attention-")
            try:
                with os.fdopen(fd, "w") as stream:
                    json.dump(current, stream)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.replace(temporary, self.path)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
            self.previous = current
        return result


def revision(feed):
    visible = {key: value for key, value in feed.items() if key not in ("revision", "sampled_at")}
    return hashlib.sha256(json.dumps(visible, sort_keys=True).encode()).hexdigest()[:32]


def wire(feed):
    lines = [["CODEXBOARD", "1"],
             ["META", feed["revision"], feed["mode"], feed["timezone"], feed["sampled_at"]],
             ["SOURCE", feed["source"]], ["MESSAGE", feed["message"]],
             ["QUOTA", feed["quota"]["status"], feed["quota"]["fetched_at"]]]
    lines.extend(["WINDOW", w["label"], str(w["remaining"]), w["resets_at"]] for w in feed["quota"]["windows"])
    lines.extend(["PANE", p["id"], p["task"], p["status"], p["event"]] for p in feed["panes"])
    lines += [["FOOTER", feed["footer"]["kind"], feed["footer"]["text"]], ["END"]]
    body = ("\n".join("\t".join(line) for line in lines) + "\n").encode("ascii")
    if len(body) > 8192:
        raise ValueError("Feed too large")
    return body
