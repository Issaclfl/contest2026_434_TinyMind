#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""用我们自己的云端模型合成中文口语语料（知识蒸馏），喂给本地意图模型。

为什么需要它：模板生成的中文只有 381 条，且"关灯"这类说法在整族留出里一条都见不到，
本地模型因此无从泛化。这里让云端模型按动作批量生成**口语说法**（含半截话、模糊表达），
再人工过一遍，直接喂现有训练脚本。

安全：key 只从被 gitignore 的 sdkconfig 读，不打印；产物只有 cmd/json/family 三字段。
用法：python distill_zh_corpus.py [--per-action 25] [--out 文件]
"""
import argparse
import io
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

SDKCONFIG = os.environ.get(
    "GATEWAY_SDKCONFIG",
    r"C:\Users\Lawson\sf32lb52_net_build\esp32s3_gateway\sdkconfig")
URL = "https://token-plan-cn.xiaomimimo.com/v1/chat/completions"
MODEL = "mimo-v2.5-pro"

# (family, 动作 JSON, 给云端模型的说明)
JOBS = [
    ("distill_zh_lamp_on",   {"action": "miio", "device": "lamp", "op": "on"},
     "打开台灯（台灯可以是\u201c灯\u201d\u201c台灯\u201d\u201c书房的灯\u201d）"),
    ("distill_zh_lamp_off",  {"action": "miio", "device": "lamp", "op": "off"},
     "关掉台灯/把灯关掉"),
    ("distill_zh_lamp_up",   {"action": "miio", "device": "lamp", "op": "brightness", "value": 80},
     "把台灯调亮一点（不要出现具体数字）"),
    ("distill_zh_lamp_down", {"action": "miio", "device": "lamp", "op": "brightness", "value": 30},
     "把台灯调暗一点/灯太刺眼（不要出现具体数字）"),
    ("distill_zh_lamp_warm", {"action": "miio", "device": "lamp", "op": "color_temp", "value": 2700},
     "把台灯调暖/调成暖光"),
    ("distill_zh_lamp_cool", {"action": "miio", "device": "lamp", "op": "color_temp", "value": 6000},
     "把台灯调冷/调成冷白光"),
    ("distill_zh_lamp_info", {"action": "miio", "device": "lamp", "op": "info"},
     "问台灯现在的状态/开着吗"),
    ("distill_zh_ac_on",     {"action": "miio", "device": "ac", "op": "on"},
     "打开空调（空调也叫\u201c冷气\u201d\u201c空调机\u201d）"),
    ("distill_zh_ac_off",    {"action": "miio", "device": "ac", "op": "off"},
     "关掉空调/把空调关了"),
]

INSTR = ("请为下面这一类意思，写 {n} 句**中文口语说法**：{desc}。"
         "要求：每句一行，越口语越好（可以有\u201c帮我\u201d\u201c麻烦\u201d\u201c一下\u201d"
         "这类词，可以有半截话和模糊表达），**不要**出现数字、不要出现\u201c空调\u201d以外的设备名、"
         "不要编号、不要解释、不要引号。只输出这些句子，每行一句。")


def load_key():
    for line in io.open(SDKCONFIG, encoding="utf-8", errors="replace"):
        if line.startswith("CONFIG_GATEWAY_CLOUD_API_KEY="):
            return line.split('"')[1]
    return None


def ask(key, desc, n):
    body = json.dumps({
        "model": MODEL,
        "messages": [{"role": "user", "content": INSTR.format(n=n, desc=desc)}],
        "temperature": 1.0,
    }, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(URL, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("Authorization", "Bearer " + key)
    with urllib.request.urlopen(req, timeout=120) as r:
        doc = json.loads(r.read().decode("utf-8", "replace"))
    return doc["choices"][0]["message"]["content"]


def clean(text):
    out = []
    for line in text.splitlines():
        s = line.strip()
        s = re.sub(r"^[\-\*\d\.、\)\s]+", "", s)          # 去掉编号/项目符号
        s = s.strip("「」“”\"'。；; ")
        if not s or len(s) > 24:
            continue
        if not any("\u4e00" <= ch <= "\u9fff" for ch in s):
            continue
        out.append(s)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-action", type=int, default=25)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                  "zh_extra.jsonl"))
    args = ap.parse_args()

    key = load_key()
    if not key:
        print("找不到 key：%s" % SDKCONFIG)
        return 2
    print("key 已读取（%d 字符，不打印）" % len(key))

    rows, seen = [], set()
    for fam, act, desc in JOBS:
        for attempt in (1, 2):
            try:
                txt = ask(key, desc, args.per_action)
                break
            except (urllib.error.HTTPError, urllib.error.URLError, KeyError, TimeoutError) as ex:
                print("  %-22s 第 %d 次失败：%s" % (fam, attempt, type(ex).__name__))
                txt = ""
                time.sleep(2)
        lines = clean(txt)
        added = 0
        for s in lines:
            if s in seen:
                continue
            seen.add(s)
            rows.append({"cmd": s, "json": json.dumps(act, ensure_ascii=False,
                                                      separators=(",", ":")), "family": fam})
            added += 1
        print("  %-22s 生成 %2d 条（%s）" % (fam, added, json.dumps(act, ensure_ascii=False)))

    with io.open(args.out, "w", encoding="utf-8", newline="\n") as fh:
        for r in rows:
            fh.write(json.dumps(r, ensure_ascii=False) + "\n")
    print("\n共 %d 条 -> %s" % (len(rows), args.out))
    print("样例：")
    for r in rows[:8]:
        print("  %-16s -> %s" % (r["cmd"], r["json"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
