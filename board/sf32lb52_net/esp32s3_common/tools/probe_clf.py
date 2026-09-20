#!/usr/bin/env python3
"""Ask the trained classifier about specific phrasings.

The held-out numbers in the training log say how the model does on average;
this says what it does with the exact sentence a person is about to say into
the board.  Run it before every hardware build:

    python3 probe_clf.py ~/qc/clf.pt 关闭台灯 打开空调 ...

With no phrasings it runs the demo's own list.
"""

import os
import sys
import zlib

import torch

NGRAMS = (1, 2, 3)
DIM = 4096

DEMO = [
    "打开台灯", "把台灯打开", "关闭台灯", "关闭客厅的灯", "关灯",
    "台灯调亮一点", "台灯暗一点", "台灯调暖一点",
    "打开空调", "关闭空调", "把空调关掉",
]


def featurize(text):
    x = torch.zeros(DIM)
    t = "".join(ch for ch in text.lower() if not ch.isspace())
    for n in NGRAMS:
        for i in range(len(t) - n + 1):
            x[zlib.crc32(t[i:i + n].encode("utf-8")) % DIM] += 1.0
    norm = float(x.norm())
    if norm > 0:
        x /= norm
    return x


def main():
    ckpt_path = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/qc/clf.pt")
    phrasings = sys.argv[2:] or DEMO

    ckpt = torch.load(ckpt_path, map_location="cpu", weights_only=False)
    labels = ckpt["labels"]
    model = torch.nn.Linear(DIM, len(labels))
    with torch.no_grad():
        model.weight.copy_(ckpt["W"])
        model.bias.copy_(ckpt["b"])
    model.eval()

    with torch.no_grad():
        for text in phrasings:
            logits = model(featurize(text).unsqueeze(0))[0]
            top = torch.topk(logits, 2)
            first = labels[top.indices[0]]
            margin = float(top.values[0] - top.values[1])
            print("%-16s -> %-52s (+%.2f)" % (text, first, margin))


if __name__ == "__main__":
    main()
