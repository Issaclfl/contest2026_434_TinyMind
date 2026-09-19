#!/usr/bin/env python3
"""把完整的 Noto Sans SC 子集化成"常用字"字体，供板子用 FreeType 渲染。

为什么不用 LVGL 自带的 lv_font_simsun_16_cjk：它只带 ASCII + 一份很偏的子集
（那串 --symbols 里日文占了大半），常用汉字缺很多，屏上就画成方框。

子集范围：
  - ASCII 0x20-0x7E
  - 常用中英标点（，。！？：；、（）《》""''…—等）
  - GB2312 一级汉字表（3755 个最常用字，用 gb2312 编解码直接枚举得到）
  - 应用里出现的专有词（TinyMind 等）

用法：python3 subset_font.py <输入.ttf> <输出.ttf>
"""

import sys

from fontTools import subset

SRC = sys.argv[1] if len(sys.argv) > 1 else (
    "/home/lawson/openvela/frameworks/graphics/uikit/test/benchmark/assets/NotoSansSC-Regular.ttf"
)
DST = sys.argv[2] if len(sys.argv) > 2 else "/tmp/NotoSansSC-sub.ttf"

PUNCT = "，。！？：；、（）《》【】“”‘’…—～·「」『』×÷±°℃%‰①-⑩"
EXTRA = "TinyMindopenvela语音播报屏喇叭联网离线天气温度湿度风速晴多云阴雨雪" \
        "打开关闭客厅红蓝绿灯就绪正在识别中芯片当前时间"

chars = set(chr(c) for c in range(0x20, 0x7F))
chars.update(PUNCT)
chars.update(EXTRA)

# GB2312 一级汉字：区 0xB0-0xD7，位 0xA1-0xFE
level1 = 0
for hi in range(0xB0, 0xD8):
    for lo in range(0xA1, 0xFF):
        try:
            ch = bytes([hi, lo]).decode("gb2312")
        except UnicodeDecodeError:
            continue
        chars.add(ch)
        level1 += 1

# 二级汉字（0xD8-0xF7）也带上，覆盖更全
level2 = 0
for hi in range(0xD8, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            ch = bytes([hi, lo]).decode("gb2312")
        except UnicodeDecodeError:
            continue
        chars.add(ch)
        level2 += 1

text = "".join(sorted(chars))
print("字符数：一级 %d + 二级 %d + ASCII/标点 = %d" % (level1, level2, len(text)))

options = subset.Options()
options.layout_features = ["*"]
options.notdef_outline = True
options.recalc_bounds = True

font = subset.load_font(SRC, options)
subsetter = subset.Subsetter(options=options)
subsetter.populate(text=text)
subsetter.subset(font)
subset.save_font(font, DST, options)
print("输出：", DST)
