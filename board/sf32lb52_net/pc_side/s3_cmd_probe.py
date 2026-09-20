#!/usr/bin/env python3
"""Ask the gateway's local command model a batch of spoken Chinese phrasings.

This exercises the offline path end to end without the board: PC -> S3
(model "cmd") -> intent classifier -> miIO client / on-board RGB led.
It is how the demo's phrasings were chosen and how the accuracy in the docs
was measured; the gateway's reply is whatever the board would speak.

Usage:
    python s3_cmd_probe.py                       # built-in battery (miio)
    python s3_cmd_probe.py --led                 # on-board RGB led battery
    python s3_cmd_probe.py --scheme http         # port 80 (the TLS listener is flaky)
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


# The on-board WS2812 (the S3's own RGB led).  Naming rule, also in
# docs/米家生态控制方案.md: 红灯/绿灯/蓝灯/白灯/黄灯 are that led, and
# RGB / 彩灯 / 板载灯 are the same led said by its own name.
LED_BATTERY = [
    ("板载灯", "打开RGB", "已打开"),
    ("板载灯", "打开RGB灯", "已打开"),
    ("板载灯", "打开彩灯", "已打开"),
    ("板载灯", "关掉RGB", "已关闭"),
    ("板载灯", "关闭RGB灯", "已关闭"),
    ("板载灯", "打开红灯", "已打开"),
    ("板载灯", "打开绿灯", "已打开"),
    ("板载灯", "打开蓝灯", "已打开"),
    ("板载灯", "打开白灯", "已打开"),
    ("板载灯", "打开黄灯", "已打开"),
    ("板载灯", "把灯调成红色", "已打开"),
    ("板载灯", "红灯闪三下", "闪了 3 下"),
    ("板载灯", "让蓝灯闪两下", "闪了 2 下"),
    ("板载灯", "红灯呼吸", "呼吸"),
]


def ask(host, phrasing, model="cmd", key="probe", timeout=30, scheme="https"):
    body = json.dumps({"model": model,
                       "messages": [{"role": "user", "content": phrasing}]})
    req = urllib.request.Request(
        "%s://%s/v1/chat/completions" % (scheme, host),
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
    ap.add_argument("--scheme", default="https", choices=["https", "http"])
    ap.add_argument("--led", action="store_true",
                    help="跑板载 RGB 灯那一组（会真的点亮/闪烁 S3 上的灯）")
    ap.add_argument("--phrasing", action="append")
    args = ap.parse_args()

    if args.phrasing:
        for p in args.phrasing:
            print("%-24s -> %s" % (p, ask(args.host, p, scheme=args.scheme)))
        return 0

    battery = LED_BATTERY if args.led else BATTERY
    passed = 0
    for device, phrasing, expect in battery:
        try:
            reply = ask(args.host, phrasing, scheme=args.scheme)
        except Exception as exc:
            reply = "ERROR: %r" % (exc,)
            # 端口 80 与 443 是同一条路由：443 那份 TLS 监听会时不时拒绝新连接
            try:
                reply = ask(args.host, phrasing, scheme="http")
            except Exception:
                pass

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
        elif expect == "ct":
            good = "色温" in reply
        elif expect == "呼吸":
            good = "呼吸" in reply
        else:
            good = expect in reply

        passed += 1 if good else 0
        print("%s %-9s %-22s -> %s" % ("ok  " if good else "MISS", device, phrasing, reply))

    total = len(battery)
    print("\n%d/%d 命中预期" % (passed, total))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
