"""Send collector snapshots to the native Badge OS USB Serial/JTAG app."""
import argparse
import time
from pathlib import Path


def snapshot(path):
    with path.open("rb") as stream:
        body = stream.read(8193)
    return validate_snapshot(body)


def validate_snapshot(body):
    if (len(body) > 8192 or not body.startswith(b"CODEXBOARD\t1\n")
            or not body.endswith(b"\nEND\n") or b"\x00" in body):
        raise ValueError("Not a complete Codex Board snapshot")
    body.decode("ascii")
    return body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--feed", type=Path, required=True)
    args = parser.parse_args()
    try:
        import serial
    except ImportError:
        parser.error("Use the existing badge Python environment with pyserial installed")
    # Set control lines before opening: do not deliberately reset the running app.
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1,
                         write_timeout=2, exclusive=True)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        port.open()
        print("USB open. Select Codex Board on the badge; Ctrl+C stops sending.", flush=True)
        pending = bytearray()
        next_send = 0
        last_error = None
        while True:
            now = time.monotonic()
            if now >= next_send:
                try:
                    body = snapshot(args.feed)
                    if port.write(body) != len(body):
                        raise serial.SerialTimeoutException("Incomplete snapshot write")
                    last_error = None
                except (OSError, ValueError) as error:
                    notice = type(error).__name__
                    if notice != last_error:
                        print("Waiting for a complete collector feed: " + notice, flush=True)
                    last_error = notice
                next_send = now + 3
            pending.extend(port.read(1024))
            while b"\n" in pending:
                line, _, rest = pending.partition(b"\n")
                pending = bytearray(rest)
                # Display only protocol acknowledgements, not arbitrary boot logs.
                text = line.strip().decode("ascii", errors="replace")
                if text == "CODEXBOARD READY" or text.startswith("CODEXBOARD ACK "):
                    print(text, flush=True)
            if len(pending) > 4096:
                pending.clear()
    except KeyboardInterrupt:
        print("\nUSB feed stopped.")
    except serial.SerialException:
        parser.exit(1, "USB disconnected or unavailable. Reconnect the device, then rerun.\n")
    finally:
        try:
            port.close()
        except OSError:
            pass


if __name__ == "__main__":
    main()
