#!/usr/bin/env python3
"""llama2.c 分词器的 Python 版：和板子上跑的 encode/decode 逐字节一致。

为什么必须自己写一个：命令模型要拿英文口令训，训练数据和设备上喂进去的东西
必须**同一个分词结果**，否则训出来的模型在板子上就是废的。上游 run.c 里
encode()/decode() 的逻辑（BPE + 字节回退 + dummy 前缀 + BOS）就是规格说明，
这里照着它写，并用往返测试钳住（见 tests 段落）。

用法：
    python3 cmd_model_bpe.py --tok ../assets/tok512.bin "turn on the red light"
    python3 cmd_model_bpe.py --tok ../assets/tok512.bin --self-test
"""
import argparse
import array
import struct
import sys


class Tokenizer:
    def __init__(self, path):
        blob = open(path, "rb").read()
        (self.max_token_length,) = struct.unpack_from("<i", blob, 0)
        off = 4
        vocab, scores = [], []
        while off < len(blob):
            (score,) = struct.unpack_from("<f", blob, off)
            (ln,) = struct.unpack_from("<i", blob, off + 4)
            off += 8
            vocab.append(blob[off:off + ln].decode("utf-8", "surrogateescape"))
            scores.append(score)
            off += ln
        self.vocab, self.scores = vocab, scores
        self.vocab_size = len(vocab)
        # 按字符串排序的索引表，str_lookup 用它做二分
        self.order = sorted(range(self.vocab_size), key=lambda i: vocab[i])

    def lookup(self, s):
        lo, hi = 0, len(self.order) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            v = self.vocab[self.order[mid]]
            if v < s:
                lo = mid + 1
            elif v > s:
                hi = mid - 1
            else:
                return self.order[mid]
        return -1

    def encode(self, text, bos=True, eos=False):
        """与 run.c 的 encode() 同序：BOS -> dummy 前缀 -> 逐码点 -> 合并循环。"""
        toks = []
        if bos:
            toks.append(1)
        if text:
            toks.append(self.lookup(" "))
        i = 0
        while i < len(text):
            # 取一个完整 UTF-8 码点（与上游一致：最多 4 字节）
            n = 1
            b = ord(text[i])
            if b >= 0xF0:
                n = 4
            elif b >= 0xE0:
                n = 3
            elif b >= 0xC0:
                n = 2
            piece = text[i:i + n]
            i += n
            tid = self.lookup(piece)
            if tid != -1:
                toks.append(tid)
            else:
                for ch in piece.encode("utf-8", "surrogateescape"):
                    toks.append(ch + 3)      # 字节回退：前 3 个是 <unk>/<s>/</s>
        # 合并：每轮挑分数最高的一对
        while True:
            best_score, best_id, best_idx = -1e10, -1, -1
            for k in range(len(toks) - 1):
                cand = self.vocab[toks[k]] + self.vocab[toks[k + 1]]
                cid = self.lookup(cand)
                if cid != -1 and self.scores[cid] > best_score:
                    best_score, best_id, best_idx = self.scores[cid], cid, k
            if best_idx == -1:
                break
            toks[best_idx] = best_id
            del toks[best_idx + 1]
        if eos:
            toks.append(2)
        return toks

    def decode(self, prev_token, token):
        piece = self.vocab[token]
        if prev_token == 1 and piece.startswith(" "):
            piece = piece[1:]
        if len(piece) == 6 and piece.startswith("<0x") and piece.endswith(">"):
            return chr(int(piece[3:5], 16))          # 字节 token
        return piece

    def decode_all(self, toks):
        """解一串 token。第一个 token 若是 BOS 则不产出文本——板子上的 generate()
        在采样到 BOS 时 break，从来不 emit 它，这里保持一致。"""
        out, prev = [], 1
        for i, t in enumerate(toks):
            if i == 0 and t == 1:
                prev = t
                continue
            out.append(self.decode(prev, t))
            prev = t
        return "".join(out)


def self_test(tok):
    cases = ["turn on the red light", "Please scan the WiFi networks.",
             '{"action":"led","color":"red"}', "关灯", "WiFi scan!!"]
    bad = 0
    for text in cases:
        ids = tok.encode(text, bos=True, eos=False)
        back = tok.decode_all(ids)
        ok = back == text
        print(f"  {'OK ' if ok else 'BAD'} {len(ids):>3} tokens  {text!r} -> {back!r}")
        bad += 0 if ok else 1
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("text", nargs="?", default=None)
    ap.add_argument("--tok", required=True, help="tokenizer.bin 路径（如 assets/tok512.bin）")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    tok = Tokenizer(a.tok)
    print(f"{a.tok}: {tok.vocab_size} tokens, max_token_length {tok.max_token_length}")
    if a.self_test:
        return 1 if self_test(tok) else 0
    if a.text is None:
        ap.error("给一段文字，或 --self-test")
    ids = tok.encode(a.text, bos=True, eos=False)
    print(f"ids: {ids}")
    print(f"back: {tok.decode_all(ids)!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
