import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from collector import finish_notifications, main, report_status_changes, tmux_rows
from model import AttentionTracker, quota_windows, timezone, title_status
from usb_feed import snapshot
from alert_test import test_feed


class CollectorTests(unittest.TestCase):
    def test_attention_survives_restart_and_changes_after_resolution(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "events.json"
            row = {"id": "%1", "task": "Layout", "status": "NEEDS_YOU"}
            first = AttentionTracker(path).update([row])[0]["event"]
            tracker = AttentionTracker(path)
            self.assertEqual(first, tracker.update([row])[0]["event"])
            tracker.update([{**row, "status": "WORKING"}])
            self.assertNotEqual(first, tracker.update([row])[0]["event"])

    def test_titles_do_not_infer_approval_or_internal_task_words(self):
        self.assertEqual(title_status("⠋ Codex: Layout | Action Required", "work"),
                         ("Layout", "NEEDS_YOU"))
        self.assertEqual(title_status("Layout [Approval Required]", "work")[1], "APPROVAL")
        self.assertEqual(title_status("Fix working directory", "work")[1], "UNKNOWN")

    def test_quota_remaining_boundary_and_local_reset(self):
        raw = {"rateLimits": {"primary": {"usedPercent": 70, "resetsAt": 0},
                              "secondary": {"usedPercent": 71, "resetsAt": 0}}}
        windows = quota_windows(raw, timezone("America/Los_Angeles"))
        self.assertEqual([(w["remaining"], w["low"]) for w in windows], [(30, False), (29, True)])
        self.assertIn("1969-12-31 16:00:00 PST -0800", windows[0]["resets_at"])

    def test_tmux_is_metadata_only_and_missing_selected_pane_fails(self):
        with patch("collector.subprocess.run") as run:
            run.return_value.stdout = "%1\twork\tLayout | Working\tcodex\n%2\tshell\tprivate\tbash\n"
            self.assertEqual(len(tmux_rows()), 1)
            self.assertNotIn("capture-pane", run.call_args.args[0])
            with self.assertRaises(ValueError):
                tmux_rows(selected=["%3"])

    def test_demo_to_usb_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            demo = root / "demo.json"
            demo.write_text(json.dumps([{"id": "one", "task": "Hello", "status": "WORKING"}]))
            with patch("sys.argv", ["collector", "--demo", str(demo), "--state-dir", directory, "--once"]):
                main()
            body = snapshot(root / "feed.txt")
            self.assertIn(b"\tDEMO\t", body)
            self.assertIn(b"PANE\tone\tHello\tWORKING\t\n", body)
            (root / "feed.txt").write_bytes(body[:-2])
            with self.assertRaises(ValueError):
                snapshot(root / "feed.txt")

    def test_all_panes_in_named_session_includes_wrappers_and_new_panes(self):
        with patch("collector.subprocess.run") as run:
            run.return_value.stdout = "%2\twork\tLayout | Working\tpython3\n"
            self.assertEqual(tmux_rows(session="project1"), [])
            self.assertEqual([r['id'] for r in tmux_rows("project1", all_panes=True)], ['%2'])
            run.return_value.stdout += "%3\tsecond\tBuild | Action Required\tcodex\n"
            rows = tmux_rows("project1", all_panes=True)
            self.assertEqual([r['id'] for r in rows], ['%2', '%3'])
            self.assertEqual(rows[1]['status'], 'NEEDS_YOU')
            command = run.call_args.args[0]
            self.assertEqual(command[2:5], ['-s', '-t', 'project1'])
            self.assertNotIn('capture-pane', command)
        with self.assertRaises(ValueError):
            tmux_rows('project1', selected=['%2'], all_panes=True)

    def test_all_sessions_with_wrappers_shells_and_linked_panes(self):
        with patch('collector.subprocess.run') as run:
            run.return_value.stdout = ('%0\tshell\tshell-host\tbash\n'
                                      '%1\twork\tReview layout | example-project\tpython3\n'
                                      '%2\tproject1\tTest parser | example-project\tpython3\n'
                                      '%2\tproject1\tTest parser | example-project\tpython3\n')
            self.assertEqual([r['id'] for r in tmux_rows(all_panes=True)], ['%0', '%1', '%2'])
            self.assertIn('-a', run.call_args.args[0])
            self.assertNotIn('-t', run.call_args.args[0])

    def test_status_before_project_becomes_one_attention_event(self):
        with tempfile.TemporaryDirectory() as directory:
            tracker = AttentionTracker(Path(directory) / 'state.json')
            with patch('collector.subprocess.run') as run:
                run.return_value.stdout = '%2\tproject1\tTest parser | Action Required | example-project\tpython3\n'
                rows = tracker.update(tmux_rows(all_panes=True))
                self.assertEqual(rows[0]['status'], 'NEEDS_YOU')
                self.assertEqual(rows[0]['task'], 'Test parser | example-project')
                self.assertEqual(len(rows[0]['event']), 32)
                repeated = tracker.update(tmux_rows(all_panes=True))
                self.assertEqual(repeated[0]['event'], rows[0]['event'])
                with patch('builtins.print') as output:
                    report_status_changes([], rows)
                    output.assert_called_once_with('Pane %2: NEEDS_YOU (LED event)', flush=True)
                    output.reset_mock()
                    report_status_changes(rows, repeated)
                    output.assert_not_called()
                run.return_value.stdout = run.return_value.stdout.replace('Action Required', 'Working')
                self.assertEqual(tracker.update(tmux_rows(all_panes=True))[0]['event'], '')

    def test_input_wording_and_non_input_updates(self):
        for title in ('Task | Input Required | project', 'Needs Input | Task | project',
                      'Task | project | Action Required'):
            self.assertEqual(title_status(title, 'fallback')[1], 'NEEDS_YOU')
        for title in ('Task | Update Required | project', 'Fix action required label | project',
                      'Task | Working | Action Required | project'):
            self.assertEqual(title_status(title, 'fallback')[1], 'UNKNOWN')

    def test_finish_notifications_initial_idle_repeat_restart_and_next_turn(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'attention.json'
            tracker = AttentionTracker(path)
            idle = [{'id': '%2', 'task': 'Test parser', 'status': 'IDLE'}]
            self.assertEqual(finish_notifications(idle)[0]['status'], 'IDLE')
            first = tracker.update(finish_notifications(idle, True))
            self.assertEqual(first[0]['status'], 'REPLY_READY')
            self.assertEqual(first[0]['event'], '')
            self.assertEqual(idle[0]['status'], 'IDLE')
            repeated = tracker.update(finish_notifications(idle, True))
            self.assertEqual(first, repeated)
            tracker = AttentionTracker(path)
            self.assertEqual(first, tracker.update(finish_notifications(idle, True)))
            working = [{**idle[0], 'status': 'WORKING'}]
            cleared = tracker.update(finish_notifications(working, True))
            self.assertEqual(cleared[0]['status'], 'WORKING')
            self.assertEqual(cleared[0]['event'], '')
            again = tracker.update(finish_notifications(idle, True))
            self.assertEqual(again[0]['status'], 'REPLY_READY')
            self.assertEqual(again[0]['event'], '')
            blocking = [{**idle[0], 'status': 'NEEDS_YOU'}]
            request = tracker.update(finish_notifications(blocking, True))
            self.assertTrue(request[0]['event'])
            self.assertEqual(request[0]['status'], 'NEEDS_YOU')
            self.assertEqual(tracker.update(finish_notifications(idle, True))[0]['event'], '')

    def test_finish_notifications_preserve_approval_and_unknown(self):
        rows = [{'id': str(i), 'status': status} for i, status in enumerate(
            ['UNKNOWN', 'WORKING', 'APPROVAL', 'NEEDS_YOU', 'IDLE', 'DONE'])]
        self.assertEqual([r['status'] for r in finish_notifications(rows, True)],
                         ['UNKNOWN', 'WORKING', 'APPROVAL', 'NEEDS_YOU', 'REPLY_READY', 'REPLY_READY'])

    def test_flash_on_ready_only_for_new_transitions_and_survives_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'attention.json'
            ready = {'id': '%2', 'task': 'Answer', 'status': 'REPLY_READY'}
            working = {**ready, 'status': 'WORKING'}
            tracker = AttentionTracker(path, flash_on_ready=True)
            self.assertEqual(tracker.update([ready])[0]['event'], '')
            tracker.update([working])
            first = tracker.update([ready])[0]['event']
            self.assertEqual(len(first), 32)
            self.assertEqual(tracker.update([ready])[0]['event'], first)
            tracker = AttentionTracker(path, flash_on_ready=True)
            self.assertEqual(tracker.update([ready])[0]['event'], first)
            tracker.update([working])
            self.assertNotEqual(tracker.update([ready])[0]['event'], first)
            self.assertEqual(AttentionTracker(path).update([ready])[0]['event'], '')

    def test_mark_ready_produces_nonblocking_badge_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            demo = root / 'demo.json'
            demo.write_text(json.dumps([{'id': '%2', 'task': 'A finished reply', 'status': 'IDLE'}]))
            command = ['collector', '--demo', str(demo), '--mark-ready',
                       '--state-dir', directory, '--once']
            with patch('sys.argv', command):
                main()
            body = snapshot(root / 'feed.txt')
            self.assertIn(b'PANE\t%2\tA finished reply\tREPLY_READY\t', body)
            event = next(line for line in body.splitlines() if line.startswith(b'PANE\t')).split(b'\t')[-1]
            self.assertEqual(event, b'')

    def test_flash_on_ready_flag_emits_event_without_urgent_status(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            demo = root / 'demo.json'
            command = ['collector', '--demo', str(demo), '--flash-on-ready',
                       '--state-dir', directory, '--once']
            demo.write_text(json.dumps([{'id': '%2', 'task': 'Answer', 'status': 'WORKING'}]))
            with patch('sys.argv', command):
                main()
            demo.write_text(json.dumps([{'id': '%2', 'task': 'Answer', 'status': 'IDLE'}]))
            with patch('sys.argv', command):
                main()
            body = snapshot(root / 'feed.txt')
            event = next(line for line in body.splitlines() if line.startswith(b'PANE\t')).split(b'\t')[-1]
            self.assertIn(b'PANE\t%2\tAnswer\tREPLY_READY\t', body)
            self.assertEqual(len(event), 32)
            with patch('sys.argv', [*command[:3], '--notify-on-finish', *command[4:]]):
                main()
            repeated = snapshot(root / 'feed.txt')
            self.assertIn(b'PANE\t%2\tAnswer\tREPLY_READY\t' + event, repeated)

    def test_weekly_only_response_omits_missing_five_hour_window(self):
        raw = {'rateLimitsByLimitId': {'codex': {
            'primary': {'usedPercent': 23, 'windowDurationMins': 10080, 'resetsAt': 1794885039},
            'secondary': None}}}
        windows = quota_windows(raw, timezone('America/Los_Angeles'))
        self.assertEqual([(w['label'], w['remaining']) for w in windows],
                         [('Weekly', 77)])
        self.assertFalse(windows[0]['low'])
        self.assertIn('PST -0800', windows[0]['resets_at'])

    def test_both_quota_windows_are_shown_without_missing_placeholders(self):
        raw = {'rateLimits': {
            'primary': {'usedPercent': 71, 'windowDurationMins': 300, 'resetsAt': 0},
            'secondary': {'usedPercent': 23, 'windowDurationMins': 10080, 'resetsAt': 100}}}
        windows = quota_windows(raw, timezone('UTC'))
        self.assertEqual([(w['label'], w['remaining']) for w in windows],
                         [('5-hour', 29), ('Weekly', 77)])

    def test_timed_alert_frames_are_fresh_and_repeat_same_event(self):
        zone = timezone('UTC')
        first = test_feed('First alert', 'a' * 32, zone, 100)
        repeat = test_feed('First alert', 'a' * 32, zone, 101)
        self.assertNotEqual(first, repeat)  # Heartbeat timestamp advances.
        self.assertEqual(first.splitlines()[1].split(b'\t')[1],
                         repeat.splitlines()[1].split(b'\t')[1])
        for body in (first, repeat):
            self.assertIn(b'PANE\tled-test\tControlled LED test\tNEEDS_YOU\t' + b'a' * 32, body)
        clear = test_feed('Clear', '', zone, 102)
        self.assertIn(b'PANE\tled-test\tControlled LED test\tWORKING\t\n', clear)
        second = test_feed('Second alert', 'b' * 32, zone, 103)
        self.assertIn(b'\tNEEDS_YOU\t' + b'b' * 32, second)


if __name__ == "__main__":
    unittest.main()
