#!/usr/bin/env python3
"""Run commands on a board's NuttX shell and print what comes back.

The console is a plain serial line, so this is just "write a line, read until
it goes quiet" -- but the quiet detection matters: a fixed sleep either
truncates long output (`ls /dev`) or wastes a second per command.

Usage:
    python nsh.py -p COM5 "ls /dev"
    python nsh.py -p COM5 -b 1000000 "uname -a" "free"
    python nsh.py -p COM5 --raw            # just show whatever arrives

The default baud is 1000000, which is what this board's console runs at.
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write("pyserial is required:  pip install pyserial\n")
    raise SystemExit(2)

# How long the line must stay silent before we call the command finished, and
# the hard ceiling for commands that never go quiet.
IDLE_SECONDS = 0.6
MAX_SECONDS = 30.0

PROMPT_HINT = b"nsh>"


def drain(port, seconds):
    """Read whatever is already in flight, for at most `seconds`."""

    deadline = time.time() + seconds
    chunks = []
    while time.time() < deadline:
        data = port.read(4096)
        if data:
            chunks.append(data)
    return b"".join(chunks)


def run_command(port, command):
    port.reset_input_buffer()

    # The readline front end wants CRLF.  A bare CR leaves the line sitting in
    # its buffer unanswered, and a bare LF reads as an empty line.
    port.write(command.encode() + b"\r\n")

    buf = bytearray()
    last_data = time.time()
    started = time.time()

    while True:
        data = port.read(4096)
        if data:
            buf.extend(data)
            last_data = time.time()

        now = time.time()
        if buf and (now - last_data) >= IDLE_SECONDS:
            break
        if (now - started) >= MAX_SECONDS:
            break

    return buf.decode("utf-8", "replace")


def clean(text, command):
    """Drop the local echo and the trailing prompt, keep the answer."""

    lines = text.replace("\r\r\n", "\n").replace("\r\n", "\n").split("\n")

    # The first line is the tty echoing the command back.
    if lines and lines[0].strip() == command.strip():
        lines = lines[1:]

    # The last non-empty line is the shell waiting for the next command.
    while lines and not lines[-1].strip():
        lines.pop()
    if lines and lines[-1].strip().endswith("nsh>"):
        lines.pop()

    # ANSI erase sequences the readline front end emits.
    out = []
    for line in lines:
        for escape in ("\x1b[K", "\x1b[0m", "\x1b[7m"):
            line = line.replace(escape, "")
        out.append(line.rstrip())

    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()

    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(
        description="Run commands on a NuttX shell over serial.")
    parser.add_argument("commands", nargs="*", help="commands to run")
    parser.add_argument("-p", "--port", required=True, help="serial port, e.g. COM5")
    parser.add_argument("-b", "--baud", type=int, default=1000000,
                        help="console baud (default 1000000)")
    parser.add_argument("--raw", action="store_true",
                        help="print whatever arrives, run nothing")
    parser.add_argument("--wait", type=float, default=2.0,
                        help="seconds to listen in --raw mode")
    args = parser.parse_args()

    port = serial.Serial(args.port, args.baud, timeout=0.1)

    if args.raw:
        sys.stdout.write(drain(port, args.wait).decode("utf-8", "replace"))
        port.close()
        return 0

    if not args.commands:
        parser.error("no commands given (or use --raw)")

    for command in args.commands:
        print(f"$ {command}")
        raw = run_command(port, command)
        body = clean(raw, command)
        print(body if body else "(no output)")
        print()

    port.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
