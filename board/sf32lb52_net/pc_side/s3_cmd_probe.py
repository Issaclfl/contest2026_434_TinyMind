#!/usr/bin/env python3
"""Ask the gateway's local command model a batch of spoken Chinese phrasings.

This exercises the offline path end to end without the board: PC -> S3
(model "cmd") -> intent classifier -> miIO client -> the device simulator.
It is how the demo's phrasings were chosen and how the accuracy in the docs
was measured; the gateway's reply is whatever the board would speak.

Usage:
    python s3_cmd_probe.py                       # built-in battery
    python s3_cmd_probe.py --host 10.138.138.122
    python s3_cmd_probe.py --phrasing 打开空调
"""

import argparse
import json
import ssl
import sys
import urllib.request

DEFAULT_HOST = "10.138.138.122"

# Phrasings a person would actually say, grouped by what they should do.
BATTERY = [
    ("台灯", "打开台灯", "on"),
    ("台灯", "把台灯打开", "on"),
    ("台灯", "开一下灯", "on"),
    ("台灯", "关闭台灯", "off"),
    ("台灯", "把台灯关掉", "off"),
    ("台灯", "关闭客厅的灯", "off"),
    ("台灯", "关灯", "off"),
    ("台灯", "台灯调亮一点", "bright"),
    ("台灯", "把台灯调到最亮", "bright"),
    ("台灯", "台灯暗一点", "dim"),
    ("台灯", "台灯调暖一点", "ct"),
    ("空调", "打开空调", "on"),
    ("空调", "把空调打开", "on"),
    ("空调", "关闭空调", "off"),
    ("空调", "把空调关掉", "off"),
]


def ask(host, phrasing, model="cmd", key="probe", timeout=30):
    body = json.dumps({"model": model,
                       "messages": [{"role": "user", "content": phrasing}]})
    req = urllib.request.Request(
        "https://%s/v1/chat/completions" % host,
        data=body.encode("utf-8"),
        headers={"Content-Type": "application/json",
                 "Authorization": "Bearer %s" % key})
    ctx = ssl._create_unverified_context()
    with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
        doc = json.loads(resp.read().decode("utf-8"))
    return doc["choices"][0]["message"]["content"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--phrasing", action="append")
    args = ap.parse_args()

    if args.phrasing:
        for p in args.phrasing:
            print("%-24s -> %s" % (p, ask(args.host, p)))
        return 0

    passed = 0
    for device, phrasing, expect in BATTERY:
        try:
            reply = ask(args.host, phrasing)
        except Exception as exc:
            reply = "ERROR: %r" % (exc,)

        # The gateway answers in the sentence the board would speak, so the
        # check is on the words, not on the action JSON.
        if expect == "on":
            good = "已打开" in reply or "开着的" in reply
        elif expect == "off":
            good = "已关闭" in reply or "关着的" in reply
        elif expect == "bright":
            good = "亮度" in reply or "最亮" in reply
        elif expect == "dim":
            good = "亮度" in reply
        else:
            good = "色温" in reply

        passed += 1 if good else 0
        print("%s %-9s %-22s -> %s" % ("ok  " if good else "MISS", device, phrasing, reply))

    total = len(BATTERY)
    print("\n%d/%d 命中预期" % (passed, total))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
