# Codex Board setup and tests

This experimental native Badge OS app uses the existing fast display driver.
It does not use MicroPython or mpremote. Distinct screen red is unavailable;
quota below 30% is marked LOW, and new LED events pulse the RGB LEDs red.
The larger-text layout shows three terminals and two quota windows per page.
Use + / - to change pages. Task names wrap across up to three lines; exact
timestamps keep their date, time, zone and UTC offset on two larger-text lines.
Physical display and LED behavior still require a hardware test for each build.

Build and flash this branch with the Badge OS ESP-IDF toolchain first. Select
**Codex Board** in the launcher and leave its Feed URL setting empty for USB.
Connect its USB Serial/JTAG port to the Linux host. If using a virtual machine,
attach the USB device to the guest through your virtualization software.
Only one program may use the serial port; stop esptool/serial consoles first.

From the repository root, start the live collector in one terminal:

```sh
python3 collector.py --all-panes --flash-on-ready
```

Use a Python environment containing pyserial for the sender. Activate your
virtual environment first, or use its Python executable in place of `python3`. The collector reads tmux titles and Codex quota, writes its feed and
event state under `~/.local/state/codex-board`, and does not download packages,
change system settings, need administrator access, or access or flash the badge.
The USB sender below is the program that opens the badge's serial port.

## USB sender

The sender reads the local feed and opens the USB serial port. It downloads no
packages, changes no files or system settings, and does not flash the badge. It
needs serial-port permission, but should run without administrator access.

In another terminal, run the sender from the repository root with the same
Python environment containing pyserial:

```sh
python3 tools/codex_board/usb_feed.py --port /dev/ttyACM0 --feed "$HOME/.local/state/codex-board/feed.txt"
```

The sender prints `CODEXBOARD ACK <revision>` when the badge accepts a snapshot.
It leaves reset control lines deasserted. If USB disconnects, reconnect it to
the host or guest and rerun the sender. Give your Linux user serial-port access
through the `dialout` group; group changes require a fresh login/session.

## Demo and hardware tests

The demo collector and alert test write feed snapshots in the local state
directory. They download nothing, need no administrator access, and do not flash
firmware; the running USB sender delivers their events to the display and LEDs.

To use the demo, stop the live collector and run this from the repository root:

```sh
python3 tools/codex_board/collector.py --demo tools/codex_board/demo.json
```

Edit `tools/codex_board/demo.json` while the collector runs. Change a pane from WORKING to
NEEDS_YOU: expect one LED sequence and a persistent INPUT NEEDED banner. Repeated frames
must not flash again. Change it back to WORKING: expect the banner to clear.
Then change it to NEEDS_YOU again: expect a new flash. Stop the sender: after
30 seconds expect a stale indication with the last pending banner preserved.
Restart the badge app while a request remains pending: expect no repeat flash.

For a controlled 40-second LED test, stop the collector, leave the USB sender
running, and run `python3 tools/codex_board/alert_test.py`
from the repository root. It uses the same feed path/lock and
generates two fresh attention events, with an 8-second lead-in before each one.
Each event is repeated for 12 seconds to verify duplicate suppression. Each new
alert flashes red for five seconds (250 ms on, 250 ms off), while the badge keeps
handling buttons and USB feeds. Repeated snapshots do not extend or restart the
sequence. Leaving the app turns the LEDs off immediately. HTTP polling waits
until the sequence ends so a network timeout cannot freeze the lights.
Expect a banner during each event and none
during the clear interval. This does not modify the collector's attention
history. Restart the live collector afterward. A protocol ACK confirms feed
acceptance, not physical LED operation; hardware observation is still required.

For a simple message test, create a text file and pass `--message-file PATH` to
the collector. Changing its contents updates the smoke-test screen; remove
that option to show the full dashboard. The smoke screen offers a manual LED test.

The normal live command includes every pane, even when its current command appears as `python3`,
and automatically includes newly created panes and sessions. Linked panes are
shown once. It also includes plain shells; inclusion alone does not establish
that Codex is running. This mode cannot be combined with `--pane` or `--demo`.
The collector prints interpreted status changes and new attention events to its
terminal so you can distinguish title parsing from badge delivery problems.
The collector reads pane titles, window names and process
names, never terminal contents or prompts. Configure titles to expose task and
status; unknown title formats remain UNKNOWN. `Action Required` is sent as
NEEDS_YOU and displayed as INPUT NEEDED; only explicit approval wording produces
APPROVAL. These rows use a dark background with light text. Title polling cannot detect
transitions that occur entirely between samples. Inspect actual Codex titles
before relying on live status classification. Status can be a whole pipe-separated
field before, after, or between the task and project fields. `Update Required`
is not treated as an input request: it could refer to a software update.

In each running Codex terminal, use `/title` and select **thread** and **status**.
Leave **spinner** off to avoid unnecessary refreshes; **project** is optional.
The picker saves defaults for future sessions; configure already-open terminals
as well. For a temporary setting in just one session, use `/title thread status`.
See [Codex terminal title settings](https://learn.chatgpt.com/docs/developer-commands#configure-terminal-title-items-with-title).
When testing a real question, leave it pending for at least ten seconds to allow
both collector and sender polling. Expect `Pane %N: NEEDS_YOU (LED event)`
in the collector, an ACK in the sender, a badge banner and one LED sequence.
Repeated snapshots should not replay it. Resolve the question and verify the
banner clears when a non-attention status is published. Hardware confirmation of
this live sequence is required even when demo events and host tests pass.

Ordinary conversational questions and completed answers can leave Codex's title
at **Idle**, without an explicit Action Required signal. With the live command
above, IDLE/DONE becomes **REPLY READY**. A new transition from another observed state
flashes the LEDs for five seconds, while the row stays nonurgent. An already-idle
pane at collector startup does not flash. Repeated snapshots and collector
restarts do not replay a ready event. This includes completed answers as well
as conversational questions; titles cannot distinguish them, and no conversation
text is inspected. Use `--mark-ready` to show REPLY READY without its LED flash.
The old `--notify-on-finish` flag is an alias for `--flash-on-ready`.

**INPUT NEEDED** means the title explicitly reports an interactive input request.
**APPROVAL** requires explicit approval wording. These two statuses generate a
persistent urgent banner and flash once per new request. Unknown titles remain
UNKNOWN. Polling every three seconds can miss transitions between samples.
When upgrading, stop the collector and USB sender, flash the matching firmware,
then start the updated collector before restarting the USB sender. Older firmware
will display REPLY READY but will not flash for its event.

Quota is read every five minutes through Codex App Server's documented
`account/rateLimits/read`, using the existing login. Unavailable quota is
marked unavailable/stale rather than invented. Only windows actually returned
by the API are displayed: a weekly-only response shows Weekly, with no missing
five-hour placeholder. If both windows are returned, both appear. Demo quota is
explicitly DEMO.
Timestamps use the host's time zone; `--timezone UTC` overrides
it. The feed includes quota fetch and reset times with zone and UTC offset.
Edit `tools/codex_board/footer.json` to change alternating quotes and jokes (one minute each).

State and atomic feed snapshots default to ~/.local/state/codex-board, outside
the repository. Attention IDs persist across collector restarts; one collector
may use each state directory. No credentials or account tokens enter the feed.
Task titles themselves can contain sensitive text, so choose suitable names.
USB snapshots share the same TSV format as the app's Wi-Fi HTTP client.
The experimental read-only HTTP server/relay is in `feed_server.py`.

Host checks from the repository root create temporary test files and C binaries
and may create Python caches. They require Python and a C compiler, but no badge,
package downloads, system changes, or administrator access:

```sh
python3 -m unittest discover -s tools/codex_board -p 'test_*.py'
sh apps/codex_board/tests/run.sh
```
