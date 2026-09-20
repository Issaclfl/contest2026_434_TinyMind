#!/usr/bin/env python3
"""生成"板载 RGB 灯"这一族的中文语料。

为什么单独一份文件：led 这一族此前**只有英文**训练数据（"make the red led
blink two times"），蒸馏出来的中文语料里一次都没提过板载灯，于是"打开RGB"
被判成了 "把台灯打开" —— 分类器不是坏了，是这个词它压根没见过。手工写的
说法单独成文件，来源在仓库里一眼可辨。

命名约定（与 docs/米家生态控制方案.md 一致）：
  红灯 / 绿灯 / 蓝灯 / 白灯 / 黄灯      -> ESP32-S3 板载 WS2812，对应颜色
  RGB / RGB灯 / 彩灯 / 板载灯 / 板子上的灯 -> 同一颗灯，点亮（白色常亮）
  台灯 / 客厅灯 / 关灯                    -> 米家台灯（miio），不受影响

输出：zh_led.jsonl（每行一个 JSON：cmd / json / family）
"""

import json
import os
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "zh_led.jsonl")

COLORS = [
    ("red", "红"),
    ("green", "绿"),
    ("blue", "蓝"),
    ("white", "白"),
    ("yellow", "黄"),
]

CN_NUM = {1: "一", 2: "两", 3: "三", 4: "四", 5: "五"}

ROWS = []


def add(family, cmd, js):
    ROWS.append((family, cmd, js))


def led(color=None, effect=None, times=None):
    d = {"action": "led"}
    if color is not None:
        d["color"] = color
    if effect is not None:
        d["effect"] = effect
    if times is not None:
        d["times"] = times
    return json.dumps(d, separators=(",", ":"), ensure_ascii=False)


# ---- 每个颜色：开 / 关 / 闪 N 下 / 呼吸 ----
for en, zh in COLORS:
    fam = "hand_zh_led_" + en
    on = led(en)
    for p in ("打开%s灯", "把%s灯打开", "%s灯打开", "开启%s灯", "把%s灯开启",
              "点亮%s灯", "开一下%s灯", "%s灯亮起来", "让%s灯亮起来"):
        add(fam + "_on", p % zh, on)

    off = led("off")
    for p in ("关闭%s灯", "把%s灯关闭", "关掉%s灯", "把%s灯关掉",
              "%s灯关闭", "关一下%s灯"):
        add(fam + "_off", p % zh, off)

    for n in (1, 2, 3, 4, 5):
        w = CN_NUM[n]
        js = led(en, "blink", n)
        add(fam + "_blink", "%s灯闪%s下" % (zh, w), js)
        add(fam + "_blink", "让%s灯闪%s下" % (zh, w), js)
        add(fam + "_blink", "%s灯闪烁%s下" % (zh, w), js)
        if n in (2, 3):
            add(fam + "_blink", "%s灯闪%d次" % (zh, n), js)

    js = led(en, "breath")
    for p in ("%s灯呼吸", "让%s灯呼吸", "%s灯做呼吸效果",
              "%s灯进入呼吸模式", "打开%s灯的呼吸效果"):
        add(fam + "_breath", p % zh, js)

    # "把灯调成红色"这类指定颜色的说法，指的也是板载灯（米家客户端只做
    # 开关/亮度/色温，没有颜色能力，不存在抢活的问题）
    for p in ("把灯调成%s色", "灯调成%s色", "把灯改成%s色"):
        add(fam + "_set", p % zh, on)

# ---- RGB / 彩灯 / 板载灯：整颗灯的说法的名字 ----
fam = "hand_zh_led_rgb"
on = led("rgb")
for p in ("打开RGB", "打开RGB灯", "把RGB灯打开", "开启RGB灯", "点亮RGB灯",
          "打开彩灯", "把彩灯打开", "开启彩灯", "点亮彩灯", "亮起彩灯",
          "打开板载灯", "打开板子上的灯", "打开板上的灯",
          "RGB灯亮起来", "让RGB灯亮起来"):
    add(fam + "_on", p, on)

off = led("off")
for p in ("关闭RGB", "关闭RGB灯", "关掉RGB", "关掉RGB灯", "把RGB灯关掉",
          "关闭彩灯", "关掉彩灯", "把彩灯关掉", "关闭板载灯"):
    add(fam + "_off", p, off)

js = led("rgb", "blink", 3)
for p in ("RGB灯闪三下", "让RGB灯闪三下", "彩灯闪三下"):
    add(fam + "_blink", p, js)

js = led("rgb", "breath")
for p in ("RGB灯呼吸", "让RGB灯呼吸", "打开RGB的呼吸效果", "彩灯呼吸"):
    add(fam + "_breath", p, js)


def main():
    seen = {}
    for family, cmd, js in ROWS:
        if cmd in seen and seen[cmd] != js:
            raise SystemExit("同一句话两个标签: %s" % cmd)
        seen[cmd] = js

    with open(OUT, "w", encoding="utf-8", newline="\n") as fh:
        for family, cmd, js in ROWS:
            fh.write(json.dumps({"cmd": cmd, "json": js, "family": family},
                                ensure_ascii=False) + "\n")
    print("%d 条 -> %s" % (len(ROWS), OUT))


if __name__ == "__main__":
    main()
