#!/usr/bin/env python3
"""Generate the hand-written half of the Chinese corpus.

The distilled half (zh_extra.jsonl) came from the cloud model and turned out
to be missing a whole verb: it has 关了 / 关一下 / 关掉 / 关空调 but never
关闭, so "关闭台灯" was classified as *turn on* — the classifier had never
seen that verb in a lamp context.  These rows are written by hand and kept in
their own file so the two provenances stay distinguishable in the repo.

Output: zh_hand.jsonl next to this script's target directory (one JSON object
per line: cmd / json / family).
"""

import json
import os
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "zh_hand.jsonl")


def act(device, op, value=None):
    d = {"action": "miio", "device": device, "op": op}
    if value is not None:
        d["value"] = value
    return json.dumps(d, separators=(",", ":"), ensure_ascii=False)


ROWS = [
    # ---- 台灯：关（正式动词 + 常见口语的补漏） ----
    ("hand_zh_lamp_off", "关闭台灯", act("lamp", "off")),
    ("hand_zh_lamp_off", "把台灯关闭", act("lamp", "off")),
    ("hand_zh_lamp_off", "关闭客厅的灯", act("lamp", "off")),
    ("hand_zh_lamp_off", "关闭客厅灯", act("lamp", "off")),
    ("hand_zh_lamp_off", "客厅灯关闭", act("lamp", "off")),
    ("hand_zh_lamp_off", "把灯关闭", act("lamp", "off")),
    ("hand_zh_lamp_off", "关闭灯", act("lamp", "off")),
    ("hand_zh_lamp_off", "麻烦关闭台灯", act("lamp", "off")),
    ("hand_zh_lamp_off", "帮我把台灯关闭", act("lamp", "off")),
    ("hand_zh_lamp_off", "台灯关上", act("lamp", "off")),
    ("hand_zh_lamp_off", "把台灯关上", act("lamp", "off")),
    ("hand_zh_lamp_off", "关一下台灯", act("lamp", "off")),
    ("hand_zh_lamp_off", "台灯关一下", act("lamp", "off")),
    ("hand_zh_lamp_off", "台灯关掉", act("lamp", "off")),
    ("hand_zh_lamp_off", "把客厅的灯关掉", act("lamp", "off")),
    # ---- 台灯：开 ----
    ("hand_zh_lamp_on", "开启台灯", act("lamp", "on")),
    ("hand_zh_lamp_on", "把台灯开启", act("lamp", "on")),
    ("hand_zh_lamp_on", "开启客厅的灯", act("lamp", "on")),
    ("hand_zh_lamp_on", "打开客厅的灯", act("lamp", "on")),
    ("hand_zh_lamp_on", "开一下台灯", act("lamp", "on")),
    ("hand_zh_lamp_on", "点亮客厅的灯", act("lamp", "on")),
    # ---- 空调：关 ----
    ("hand_zh_ac_off", "关闭空调", act("ac", "off")),
    ("hand_zh_ac_off", "把空调关闭", act("ac", "off")),
    ("hand_zh_ac_off", "空调关闭", act("ac", "off")),
    ("hand_zh_ac_off", "把空调关上", act("ac", "off")),
    ("hand_zh_ac_off", "空调关上", act("ac", "off")),
    ("hand_zh_ac_off", "关一下空调", act("ac", "off")),
    ("hand_zh_ac_off", "麻烦关闭空调", act("ac", "off")),
    ("hand_zh_ac_off", "把空调关掉", act("ac", "off")),
    # ---- 空调：开 ----
    ("hand_zh_ac_on", "开启空调", act("ac", "on")),
    ("hand_zh_ac_on", "把空调开启", act("ac", "on")),
    ("hand_zh_ac_on", "开一下空调", act("ac", "on")),
    ("hand_zh_ac_on", "空调开一下", act("ac", "on")),
    ("hand_zh_ac_on", "打开空调", act("ac", "on")),

    # ---- 极值：调到最亮 / 最暗（"最"字句，第一版训练出来方向是反的） ----
    ("hand_zh_lamp_max", "把台灯调到最亮", act("lamp", "brightness", 100)),
    ("hand_zh_lamp_max", "台灯调到最亮", act("lamp", "brightness", 100)),
    ("hand_zh_lamp_max", "台灯开到最亮", act("lamp", "brightness", 100)),
    ("hand_zh_lamp_max", "开到最亮", act("lamp", "brightness", 100)),
    ("hand_zh_lamp_max", "调到最亮", act("lamp", "brightness", 100)),
    ("hand_zh_lamp_max", "把灯调到最大亮度", act("lamp", "brightness", 100)),
    ("hand_zh_lamp_max", "台灯亮度调到最大", act("lamp", "brightness", 100)),
    ("hand_zh_lamp_max", "灯调到最亮", act("lamp", "brightness", 100)),
    ("hand_zh_lamp_min", "台灯调到最暗", act("lamp", "brightness", 20)),
    ("hand_zh_lamp_min", "把台灯调到最暗", act("lamp", "brightness", 20)),
    ("hand_zh_lamp_min", "台灯开到最暗", act("lamp", "brightness", 20)),
    ("hand_zh_lamp_min", "调到最暗", act("lamp", "brightness", 20)),
    ("hand_zh_lamp_min", "把灯调到最低亮度", act("lamp", "brightness", 20)),
    ("hand_zh_lamp_min", "台灯亮度调到最低", act("lamp", "brightness", 20)),
    # ---- 百分比说法 ----
    ("hand_zh_lamp_pct", "台灯调到百分之五十", act("lamp", "brightness", 50)),
    ("hand_zh_lamp_pct", "把台灯调到百分之五十", act("lamp", "brightness", 50)),
    ("hand_zh_lamp_pct", "台灯亮度百分之五十", act("lamp", "brightness", 50)),
    ("hand_zh_lamp_pct", "台灯调到百分之八十", act("lamp", "brightness", 80)),
    ("hand_zh_lamp_pct", "台灯调到百分之二十", act("lamp", "brightness", 20)),
    ("hand_zh_lamp_pct", "台灯调到百分之三十", act("lamp", "brightness", 30)),
    ("hand_zh_lamp_pct", "台灯调到百分之百", act("lamp", "brightness", 100)),
    # ---- 抱怨式（"太暗了"= 要更亮，"太亮了"= 要更暗）----
    ("hand_zh_lamp_complain", "灯太暗了", act("lamp", "brightness", 80)),
    ("hand_zh_lamp_complain", "台灯太暗了", act("lamp", "brightness", 80)),
    ("hand_zh_lamp_complain", "太暗了，调亮一点", act("lamp", "brightness", 80)),
    ("hand_zh_lamp_complain", "灯太亮了", act("lamp", "brightness", 30)),
    ("hand_zh_lamp_complain", "台灯太亮了", act("lamp", "brightness", 30)),
]


def main():
    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        for family, cmd, js in ROWS:
            fh.write(json.dumps({"cmd": cmd, "json": js, "family": family},
                                ensure_ascii=False) + "\n")
    print("%d 条 -> %s" % (len(ROWS), OUT))


if __name__ == "__main__":
    main()
