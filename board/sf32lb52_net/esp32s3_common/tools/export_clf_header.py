#!/usr/bin/env python3
"""把训练好的分类器导出成 C 头（int8 权重 + 标签表），给 S3 侧推理用。

为什么能这么小：推理只要 argmax，(a) 整矩阵乘一个正数不影响 argmax → 量化即可；
(b) 输入是字符 n-gram 的计数袋，一句中文只有十几到几十个非零特征 → 点积是稀疏的，
C 侧只需对这些非零项做乘加，不需要任何向量化技巧。
"""
import argparse, io, os, sys
import torch

ap = argparse.ArgumentParser()
ap.add_argument("--ckpt", default=os.path.expanduser("~/qc/clf.pt"))
ap.add_argument("--out", default=os.path.expanduser("~/qc/clf_weights.h"))
a = ap.parse_args()

d = torch.load(a.ckpt, map_location="cpu")
W, b, labels, dim = d["W"].float(), d["b"].float(), d["labels"], int(d["dim"])
classes = W.shape[0]
print("类别 %d / 维度 %d" % (classes, dim))

scale = float(W.abs().max()) / 127.0 or 1.0
Wq = torch.round(W / scale).clamp(-127, 127).to(torch.int8).numpy()
bq = (b / scale).numpy()

def cesc(s):
    return s.replace(chr(92), chr(92)*2).replace(chr(34), chr(92)+chr(34))

L = []
L.append("/* 自动生成，请勿手改：由 tools/export_clf_header.py 从 ~/qc/clf.pt 导出。")
L.append(" * 意图分类器（字符 1-3 gram 哈希 + 线性 softmax，int8 量化）。")
L.append(" * 推理只要 argmax：整矩阵乘正数不影响 argmax，所以量化安全；")
L.append(" * 输入一句中文只有十几到几十个非零特征，点积稀疏、毫秒级。 */")
L.append("#pragma once")
L.append("")
L.append("#define CLF_DIM %d" % dim)
L.append("#define CLF_CLASSES %d" % classes)
L.append("#define CLF_SCALE %rf" % scale)
L.append("")
L.append("static const char *const CLF_LABELS[CLF_CLASSES] = {")
for s in labels:
    L.append("    %s%s%s," % (chr(34), cesc(s), chr(34)))
L.append("};")
L.append("")
L.append("static const signed char CLF_W[CLF_CLASSES][CLF_DIM] = {")
for r in range(classes):
    L.append("{" + ",".join(str(int(v)) for v in Wq[r]) + "},")
L.append("};")
L.append("")
L.append("static const float CLF_B[CLF_CLASSES] = {")
L.append("    " + ",".join("%.6ff" % v for v in bq) + ",")
L.append("};")
io.open(a.out, "w", encoding="utf-8", newline=chr(10)).write(chr(10).join(L) + chr(10))
print("写出 %s (%.1f KB)" % (a.out, os.path.getsize(a.out)/1024.0))
