#!/usr/bin/env python3
"""意图分类器：字符 n-gram 哈希 + 线性 softmax —— 为 S3 准备的小模型（第一步：先拿到数字）。

为什么换形态：闭集意图分类用分类器比用生成式小模型（0.26M 的 llama2.c）更省参数、
更准。输入是一句中文/英文说法，输出是**闭集动作 JSON**（和现在网关对板子的接口一致，
所以板子一行都不用改）。特征用字符 1-3 gram 的稳定哈希（crc32，不用 Python 的
hash——它每个进程都不一样），权重是 DIM x 类别数 的矩阵，int8 量化后只有几百 KB。

纪律（与语料生成脚本一致）：中文留出集才是要看的那个数，
**中文留出集 ≥ 90% 才考虑上板**；同时把"开/关""亮/暗"两对混淆单独打出来。

用法：python3 train_intent_clf.py [数据目录] [--epochs N]
"""
import argparse
import collections
import json
import os
import sys
import zlib

import torch

NGRAMS = (1, 2, 3)
DIM = 4096
HELD_STRONG = ("zh_off", "zh_bright_dim")


def featurize(text):
    """字符 1-3 gram 的哈希袋（L2 归一化后交给线性层）。"""
    x = torch.zeros(DIM)
    t = "".join(ch for ch in text.lower() if not ch.isspace())
    for n in NGRAMS:
        for i in range(len(t) - n + 1):
            h = zlib.crc32(t[i:i + n].encode("utf-8")) % DIM
            x[h] += 1.0
    norm = float(x.norm())
    if norm > 0:
        x /= norm
    return x


def is_zh(s):
    return any("\u4e00" <= ch <= "\u9fff" for ch in s)


def load(path):
    rows = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data", nargs="?", default=os.path.expanduser("~/qc/cmd_data_zh"))
    ap.add_argument("--epochs", type=int, default=400)
    ap.add_argument("--lr", type=float, default=0.5)
    ap.add_argument("--seed", type=int, default=1337)
    ap.add_argument("--save", default=os.path.expanduser("~/qc/clf.pt"))
    ap.add_argument("--extra", action="append", default=[], metavar="JSONL",
                    help="额外语料（{cmd,json} 每行一条，可给多次）；只进训练集\n"
                         "留出集不变，所以两种来源都能在统计里对得上")
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    train = load(os.path.join(args.data, "train.jsonl"))
    held = load(os.path.join(args.data, "heldout.jsonl"))
    for extra in args.extra:
        rows = load(extra)
        train.extend(rows)
        print("额外语料 %s：%d 条 -> 训练集" % (extra, len(rows)))
    labels = sorted({r["json"] for r in train + held})
    idx = {lab: i for i, lab in enumerate(labels)}
    print("训练 %d 条 / 留出 %d 条 / 类别 %d" % (len(train), len(held), len(labels)))

    X = torch.stack([featurize(r["cmd"]) for r in train])
    Y = torch.tensor([idx[r["json"]] for r in train])
    HX = torch.stack([featurize(r["cmd"]) for r in held]) if held else torch.zeros(0, DIM)
    HY = torch.tensor([idx[r["json"]] for r in held])

    model = torch.nn.Linear(DIM, len(labels))
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    lossf = torch.nn.CrossEntropyLoss()
    for ep in range(args.epochs):
        opt.zero_grad()
        loss = lossf(model(X), Y)
        loss.backward()
        opt.step()
        if (ep + 1) % 100 == 0:
            acc = (model(X).argmax(1) == Y).float().mean().item()
            print("  epoch %3d  loss %.4f  训练集 %.1f%%" % (ep + 1, loss.item(), 100 * acc))

    with torch.no_grad():
        pred = model(HX).argmax(1)
    hits = (pred == HY)
    print("\n=== 留出集 ===")
    print("总体: %d/%d = %.1f%%" % (hits.sum().item(), len(held), 100 * hits.float().mean().item()))

    zh = [i for i, r in enumerate(held) if is_zh(r["cmd"])]
    if zh:
        h = sum(1 for i in zh if hits[i])
        print("**中文留出集: %d/%d = %.1f%%**  （门槛 90%%）" % (h, len(zh), 100 * h / len(zh)))
    for fam in HELD_STRONG:
        sub = [i for i, r in enumerate(held) if r.get("family") == fam]
        if sub:
            h = sum(1 for i in sub if hits[i])
            print("整族留出 %-14s: %d/%d" % (fam, h, len(sub)))

    print("\n=== 逐条（留出集）===")
    for i, r in enumerate(held):
        flag = "OK " if hits[i] else "错 "
        if not hits[i]:
            print("%s %-24s 期望 %-46s 实得 %s" % (flag, r["cmd"], r["json"], labels[int(pred[i])]))
    print("（只列错的；对的 %d 条省略）" % int(hits.sum().item()))

    conf = collections.Counter()
    for i, r in enumerate(held):
        if not hits[i]:
            want, got = r["json"], labels[int(pred[i])]
            def op(js):
                try:
                    d = json.loads(js)
                except Exception:
                    return js
                return d.get("op") or d.get("action") or "?"
            conf[(op(want), op(got))] += 1
    if conf:
        print("\n=== 混淆对（期望 -> 实得）===")
        for (a, b), n in conf.most_common(10):
            print("  %-12s -> %-12s x%d" % (a, b, n))
        pairs = [((a, b), n) for (a, b), n in conf.items()
                 if {a, b} & {"on", "off"} or {a, b} & {"brightness"}]
        print("开/关 或 亮度 相关混淆: %d 处" % sum(n for (_, _), n in
              [((a, b), n) for (a, b), n in conf.items() if {a, b} & {"on", "off", "brightness"}]))
    torch.save({"W": model.weight.detach(), "b": model.bias.detach(),
                "labels": labels, "dim": DIM, "ngrams": NGRAMS}, args.save)
    print(chr(10) + "权重已存: " + args.save)
    return 0


if __name__ == "__main__":
    sys.exit(main())
