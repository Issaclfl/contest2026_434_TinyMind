#!/usr/bin/env python3
"""造命令模型的数据集：英文口令 -> 固定 JSON 动作。

为什么要"固定 JSON"而不是自由文本：目标是让一个 0.26M~1M 参数的小模型**可靠地
控制硬件**。开放输出（自由文本、任意 URL、任意 IP）是小模型最容易翻车的地方，
所以动作空间做成闭集：每个槽位的取值都是枚举好的，模型只要学会"这句话对应哪个
动作"，执行侧再把枚举翻译成真实动作（比如 gateway -> 10.0.0.1）。

动作表（唯一真源，执行侧照它实现）：

    {"action":"led","color":"red|green|blue|white|yellow|off"}
    {"action":"wifi_scan"}
    {"action":"ping","target":"gateway|internet|board"}
    {"action":"status"}

训练样本是"说法"到"JSON"的映射，句式由模板库组合生成；**留出的那部分说法按
"动词族"整族切分**，所以留出集测的是"换个说法它还认不认"，不是"背没背下来"。

输出：指令用的 JSONL 行（turn/文本/json 三个字段）+ 供人看的统计。
用法：
    python3 tools/make_cmd_dataset.py --out ~/qc/cmd_data
"""
import argparse
import json
import os
import random
import sys

# ---- 动作表 ----------------------------------------------------------------
LED_COLORS = ["red", "green", "blue", "white", "yellow"]


def actions():
    a = []
    for c in LED_COLORS:
        a.append({"action": "led", "color": c})
    a.append({"action": "led", "color": "off"})
    a.append({"action": "wifi_scan"})
    for t in ["gateway", "internet", "board"]:
        a.append({"action": "ping", "target": t})
    a.append({"action": "status"})
    return a


def canon(a):
    """唯一写法：紧凑、键序固定（模型要学的就是这个字符串）。"""
    return json.dumps(a, separators=(",", ":"), ensure_ascii=False)


# ---- 说法模板：按"动词族"分组，留出集整族留出 --------------------------------
# 每个动词族给出 (需要颜色?, 需要 ping 目标?, 模板列表)，占位符 {c} 颜色 {t} 目标。
FAMILIES = {
    "turn": [
        (True, False, ["turn on the {c} light", "turn the {c} light on",
                       "turn on the {c} led", "please turn on the {c} light",
                       "can you turn on the {c} light"]),
        (False, False, ["turn off the light", "turn the light off",
                        "please turn off the light", "turn off the led"]),
    ],
    "switch": [
        (True, False, ["switch the {c} light on", "switch on the {c} led",
                       "switch the led to {c}"]),
        (False, False, ["switch the light off", "switch off the led"]),
    ],
    "make": [
        (True, False, ["make the light {c}", "make the led {c}",
                       "make it {c}"]),
    ],
    "set": [
        (True, False, ["set the led to {c}", "set the light to {c}",
                       "set the led colour to {c}", "set colour {c}"]),
    ],
    "bare": [
        (True, False, ["{c} light", "{c} light on", "{c} led on",
                       "{c} please", "lights {c}"]),
        (False, False, ["lights off", "light off", "led off", "off"]),
    ],
    "scan": [
        (False, False, ["scan the wifi networks", "scan wifi", "do a wifi scan",
                        "list the wifi networks", "what wifi networks are there",
                        "show me available wifi"]),
    ],
    "ping": [
        (False, True, ["ping the {t}", "ping {t}", "test the connection to the {t}",
                       "can you ping the {t}", "check the {t} connection"]),
    ],
    "status": [
        (False, False, ["what is your status", "report status", "show your status",
                        "how are you connected", "status please", "are you online"]),
    ],
}

# 留出整族：模型没见过这几种说法，测的是泛化
HELD_OUT = {"bare", "switch"}

# ---- 第二批动作（2026-09-19）：读传感器、取网络数据、转云端问答 ---------------
# 这些动作族的说法**没法整族留出**（一族就一种活），所以留出的粒度是"每组最后一个
# 说法"：训练集里见不到它，留出集里出现。诚实地说，这比"整族留出"弱一档。
LED_TIMES = [2, 3, 4, 5]
WORD_TIMES = {"two": 2, "three": 3, "four": 4, "five": 5}
CITIES = ["beijing", "shanghai", "guangzhou", "shenzhen",
          "hangzhou", "chengdu", "wuhan", "xian"]

# 每个族：(模板列表, 由 (颜色, 次数, 城市) 造动作, 组合生成器)
NEW_FAMILIES = {
    "blink": (["blink the {c} light {n} times", "blink {c} {n} times",
               "flash the {c} led {n} times", "make the {c} light blink {n} times"],
              lambda c, n, city: {"action": "led", "color": c, "effect": "blink", "times": n},
              lambda: [(c, n, None) for c in LED_COLORS for n in LED_TIMES]),
    "breath": (["breathe the {c} light", "make the {c} led breathe",
                "let the {c} light breathe", "{c} light breathing",
                "fade the {c} light in and out"],
               lambda c, n, city: {"action": "led", "color": c, "effect": "breath"},
               lambda: [(c, None, None) for c in LED_COLORS]),
    "temperature": (["what is the chip temperature", "how hot is the chip",
                     "read the temperature", "what is your temperature",
                     "check the chip temperature", "temperature please",
                     "how warm is the chip running"],
                    lambda c, n, city: {"action": "temperature"},
                    lambda: [(None, None, None)]),
    "time": (["what time is it", "tell me the time", "what is the time now",
              "do you know the time", "current time please", "give me the time",
              "what o clock is it"],
             lambda c, n, city: {"action": "time"},
             lambda: [(None, None, None)]),
    "weather": (["what is the weather in {city}", "weather in {city}",
                 "how is the weather in {city}", "is it raining in {city}",
                 "what is the temperature outside in {city}",
                 "tell me the weather in {city}"],
                lambda c, n, city: {"action": "weather", "city": city},
                lambda: [(None, None, city) for city in CITIES]),
    # 通用问答：本地小模型只判断"这题该去问云端"。这些句子刻意**不含**设备名词、
    # 也不含 time / weather / temperature 那几族的关键词——那几种各有各的动作。
    "ask": (["what is a microcontroller", "tell me a joke", "who wrote hamlet",
             "explain what ppp means", "how many people live in china",
             "what is the capital of france", "why is the sky blue",
             "what does ram stand for", "give me one fun fact about space",
             "who invented the transistor", "what is a neural network",
             "how tall is mount everest", "what language do they speak in brazil",
             "why do we dream"],
            lambda c, n, city: {"action": "ask"},
            lambda: [(None, None, None)]),
}

HELD_OUT |= set(NEW_FAMILIES) | {"blinkw"}

# ---- 中文说法 + 米家动作（2026-09-20）----------------------------------------
# 两条原则：
#   1) **输出永远是 ASCII 的闭集 JSON**，中文只出现在输入侧；设备在 JSON 里是逻辑名
#      （lamp），由 S3 的设备表映射到真实 IP/token，板子不参与。
#   2) 中文的留出集才是演示真正要看的那个数，所以中文族单独命名（zh_*）：
#      整族留出几个，其余族留出"最后一个说法"（后者比整族弱一档，如实标注）。
#
# 模糊表达**刻意**进语料：小模型必须学会"调暗一点 -> 30"这种映射，
# 而不是指望用户报数字。
ZH_LAMP = ["台灯", "灯"]
# 整族留出的中文族：模型完全没见过这些说法。演示是中文的，所以
# **中文留出集的准确率才是要看的那个数**，其余中文族只留出"最后一个说法"。
ZH_STRONG_HELD = {"zh_off", "zh_bright_dim"}
HELD_OUT |= ZH_STRONG_HELD
MIIO_DEV = "lamp"
EN_DEV = "lamp"


def miio(op, **kw):
    a = {"action": "miio", "device": MIIO_DEV, "op": op}
    a.update(kw)
    return a


def en_miio(op, **kw):
    a = {"action": "miio", "device": EN_DEV, "op": op}
    a.update(kw)
    return a


# 每族：(模板列表, 造动作, 槽位组合)。模板里 {d} 设备词、{v} 数值。
ZH_FAMILIES = {
    "zh_on": (["打开{d}", "把{d}打开", "开一下{d}", "帮我打开{d}", "把{d}点开"],
              lambda d=None, v=None: miio("on"),
              lambda: [(d, None) for d in ZH_LAMP]),
    "zh_off": (["关掉{d}", "把{d}关掉", "关一下{d}", "把{d}关了"],
               lambda d=None, v=None: miio("off"),
               lambda: [(d, None) for d in ZH_LAMP]),
    "zh_toggle": (["{d}切换一下", "把{d}切一下", "{d}给我切一下"],
                  lambda d=None, v=None: miio("toggle"),
                  lambda: [(d, None) for d in ZH_LAMP]),
    # 数字形态：学"数到值"的对应
    "zh_bright": (["把{d}亮度调到{v}", "亮度调到{v}", "{d}亮度设成{v}", "把{d}调到{v}"],
                  lambda d=None, v=None: miio("brightness", value=v),
                  lambda: [(d, v) for d in ZH_LAMP for v in [20, 30, 50, 80, 100]]),
    # 模糊形态：学"调暗一点 -> 30"这类映射（演示时最可能出现的就是这种话）
    "zh_bright_dim": (["调暗一点", "{d}调暗一点", "把{d}调暗点", "{d}太亮了"],
                      lambda d=None, v=None: miio("brightness", value=30),
                      lambda: [(d, None) for d in ZH_LAMP]),
    "zh_bright_up": (["亮一点", "{d}亮一点", "把{d}调亮点", "{d}有点暗"],
                     lambda d=None, v=None: miio("brightness", value=80),
                     lambda: [(d, None) for d in ZH_LAMP]),
    "zh_ct": (["把{d}色温调到{v}", "色温设成{v}", "{d}色温{v}"],
              lambda d=None, v=None: miio("color_temp", value=v),
              lambda: [(d, v) for d in ZH_LAMP for v in [2700, 4000, 6000]]),
    # 色温的人话（"调暖""冷白"）——与亮度那两组一样，考的是"词到值"的映射
    "zh_ct_warm": (["把{d}调暖一点", "{d}调暖", "把{d}调成暖光"],
                   lambda d=None, v=None: miio("color_temp", value=2700),
                   lambda: [(d, None) for d in ZH_LAMP]),
    "zh_ct_cool": (["把{d}调冷一点", "{d}调冷白", "把{d}调成冷光"],
                   lambda d=None, v=None: miio("color_temp", value=6000),
                   lambda: [(d, None) for d in ZH_LAMP]),
    "zh_info": (["{d}现在什么状态", "看看{d}的状态", "{d}开着吗", "{d}状态怎么样"],
                lambda d=None, v=None: miio("info"),
                lambda: [(d, None) for d in ZH_LAMP]),
}

# 英文也给一份米家动作（英文模型那条线同样受益，且便于对照）
EN_MIIO = {
    "mm_on": (["turn on the desk lamp", "switch the lamp on", "lamp on"],
              lambda: en_miio("on"), lambda: [(None, None)]),
    "mm_off": (["turn off the desk lamp", "switch the lamp off", "lamp off"],
               lambda: en_miio("off"), lambda: [(None, None)]),
    "mm_bright": (["set the lamp brightness to {v}", "dim the lamp to {v} percent"],
                  lambda: None, lambda: [(v, None) for v in [20, 30, 50, 80, 100]]),
    "mm_ct": (["make the lamp warmer", "set the lamp to warm white"],
              lambda: en_miio("color_temp", value=2700), lambda: [(None, None)]),
}

# 中文侧的既有动作（状态/时间/天气/温度/该问云端），演示用得到
ZH_LOCAL = {
    "zh_status": (["你现在的状态", "系统状态怎么样", "报一下状态"],
                  lambda: {"action": "status"}, lambda: [(None,)]),
    "zh_time": (["现在几点", "几点了", "现在什么时间"],
                lambda: {"action": "time"}, lambda: [(None,)]),
    "zh_temperature": (["芯片温度多少", "芯片热不热", "现在的温度"],
                       lambda: {"action": "temperature"}, lambda: [(None,)]),
    "zh_weather": (["{city}天气怎么样", "今天{city}天气", "{city}下雨吗"],
                   lambda: None, lambda: [(c,) for c in ["北京", "上海", "广州", "深圳"]]),
    "zh_ask": (["讲个笑话", "什么是单片机", "珠穆朗玛峰有多高", "为什么天是蓝的"],
               lambda: {"action": "ask"}, lambda: [(None,)]),
}


def target_words(t):
    return {"gateway": ["gateway", "router"], "internet": ["internet", "web"],
            "board": ["board", "devkit"]}[t]


def build():
    items = []
    for fam, blocks in FAMILIES.items():
        for needs_color, needs_target, tmpls in blocks:
            for tmpl in tmpls:
                if needs_color:
                    for c in LED_COLORS:
                        items.append((fam, tmpl.format(c=c), {"action": "led", "color": c}))
                elif needs_target:
                    for t in ["gateway", "internet", "board"]:
                        for w in target_words(t):
                            items.append((fam, tmpl.format(t=w),
                                          {"action": "ping", "target": t}))
                else:
                    act = {"action": "wifi_scan"} if fam == "scan" else (
                        {"action": "status"} if fam == "status" else {"action": "led", "color": "off"})
                    if fam == "turn" and "on" in tmpl:
                        # "turn on the light" 这类没点名颜色的，归到白灯
                        act = {"action": "led", "color": "white"}
                    items.append((fam, tmpl, act))
    # "turn on the light"（无颜色）单独补一批：归白灯
    for tmpl in ["turn on the light", "turn the light on", "light on please",
                 "please turn on the light"]:
        items.append(("turn", tmpl, {"action": "led", "color": "white"}))

    # 数字的词形（"three times"）单独补一批。语音那条路上 ASR 转写出来的是**词**
    # 而不是数字——实测 "blink the blue light three times" 逐字转写，而第一批只训了
    # "3 times"，于是语音说这句会翻车（真机上试出来才知道）。
    global _HELD_OUT_EXTRA
    for i, tmpl in enumerate(["blink the {c} light {w} times", "blink {c} {w} times",
                              "make the {c} led blink {w} times",
                              "flash the {c} light {w} times"]):
        family = "blinkw" if i == 3 else "blinkw_tr"
        for c in LED_COLORS:
            for w, n in WORD_TIMES.items():
                items.append((family, tmpl.format(c=c, w=w),
                              {"action": "led", "color": c, "effect": "blink", "times": n}))

    # 第二批动作：最后一个说法留出，其余进训练
    for fam, (tmpls, make_act, combos) in NEW_FAMILIES.items():
        for i, tmpl in enumerate(tmpls):
            family = fam if i == len(tmpls) - 1 else fam + "_tr"
            for c, n, city in combos():
                text = tmpl.format(c=c, n=n, city=city) if ("{" in tmpl) else tmpl
                items.append((family, text, make_act(c, n, city)))
    # 中文说法 + 米家动作：每族留出"最后一个说法"（weak held-out），
    # zh_off / zh_bright_dim 整族留出（strong held-out，演示要看的那个数）
    for fam, (tmpls, make_act, combos) in ZH_FAMILIES.items():
        strong = fam in ZH_STRONG_HELD
        for i, tmpl in enumerate(tmpls):
            if strong:
                family = fam          # 整族进留出集
            else:
                family = fam if i == len(tmpls) - 1 else fam + "_tr"
            for d, v in combos():
                items.append((family, tmpl.format(d=d, v=v), make_act(d, v)))

    for fam, (tmpls, make_act, combos) in EN_MIIO.items():
        for i, tmpl in enumerate(tmpls):
            family = fam if i == len(tmpls) - 1 else fam + "_tr"
            for a, b in combos():
                if fam == "mm_bright":
                    text = tmpl.format(v=a)
                    items.append((family, text, en_miio("brightness", value=a)))
                else:
                    items.append((family, tmpl, make_act()))

    for fam, (tmpls, make_act, combos) in ZH_LOCAL.items():
        for i, tmpl in enumerate(tmpls):
            family = fam if i == len(tmpls) - 1 else fam + "_tr"
            for (city,) in combos():
                if fam == "zh_weather":
                    items.append((family, tmpl.format(city=city),
                                  {"action": "weather", "city": city}))
                else:
                    items.append((family, tmpl, make_act()))

    return items


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.expanduser("~/qc/cmd_data"),
                    help="输出目录（会写 train.jsonl / heldout.jsonl / stats.txt）")
    ap.add_argument("--augment", type=int, default=2,
                    help="每个说法再生成几条加噪变体（大小写/多余空格/问号），0 = 不加")
    ap.add_argument("--seed", type=int, default=1337)
    a = ap.parse_args()
    rnd = random.Random(a.seed)

    items = build()
    train, held = [], []
    for fam, text, act in items:
        (held if fam in HELD_OUT else train).append({"cmd": text, "json": canon(act),
                                                     "family": fam})

    def augment(rows):
        out = []
        for r in rows:
            out.append(r)
            t = r["cmd"]
            if any("一" <= ch <= "鿿" for ch in t):
                # 中文：写法差异（句读、语气词、空格）—— ASR 转写通常不带标点
                variants = [t + "。", t + "，谢谢", "请" + t, t + "吧",
                            "  " + t + "  ", t.replace("，", "")]
            else:
                variants = [t.capitalize(), t.upper() if rnd.random() < 0.15 else t,
                            t + "?", t + " please", "  " + t + "  ", t.replace(" ", "  ")]
            for v in rnd.sample(variants, min(a.augment, len(variants))):
                out.append({"cmd": v, "json": r["json"], "family": r["family"]})
        return out

    if a.augment:
        train = augment(train)
    # 打乱（训练的先后顺序不该有关系）
    rnd.shuffle(train)
    rnd.shuffle(held)

    os.makedirs(a.out, exist_ok=True)
    for name, rows in (("train.jsonl", train), ("heldout.jsonl", held)):
        with open(os.path.join(a.out, name), "w", encoding="utf-8") as f:
            for r in rows:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")

    acts = {}
    for r in train + held:
        acts[r["json"]] = acts.get(r["json"], 0) + 1
    def is_zh(t):
        return any("一" <= ch <= "鿿" for ch in t)

    zh_tr = sum(1 for r in train if is_zh(r["cmd"]))
    zh_he = sum(1 for r in held if is_zh(r["cmd"]))
    stats = [f"训练 {len(train)} 条（中文 {zh_tr}） / 留出 {len(held)} 条（中文 {zh_he}）",
             f"整族留出：{sorted(HELD_OUT)}",
             f"动作种类 {len(acts)}"]
    for k in sorted(acts):
        stats.append(f"  {acts[k]:>4}  {k}")
    text = "\n".join(stats)
    print(text)
    with open(os.path.join(a.out, "stats.txt"), "w", encoding="utf-8") as f:
        f.write(text + "\n")
    print(f"\n写出 {a.out}/train.jsonl, {a.out}/heldout.jsonl, {a.out}/stats.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
