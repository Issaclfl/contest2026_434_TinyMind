#!/usr/bin/env python3
"""把 stories260K 微调成"命令模型"：英文口令 -> 固定 JSON 动作。

为什么从一个讲故事的小模型开始，而不是从零训：它已经认识英文词和标点，而我们
只有几百条命令样本——从零训一个 0.26M 模型在这种数据量下学不出东西，接着它微调
则很快收敛（而且它就在我们板子上跑着，格式、分词器、量化器全都是现成的）。

任务形式（和板子上推理时一模一样）：

    cmd: turn on the red light\\n    ->    {"action":"led","color":"red"}\\n<s>

推理时板子喂 "cmd: <口令>\\n"，让它续写，遇到 BOS(=1) 就停；所以训练样本末尾
也要放一个 BOS，模型才会学会"说完 JSON 就停"。

要求 torch（CPU 版即可）：
    python3 -m venv ~/qc/venv && ~/qc/venv/bin/pip install torch --index-url https://download.pytorch.org/whl/cpu
    ~/qc/venv/bin/python tools/train_cmd_model.py --data ~/qc/cmd_data --steps 1500

产出：assets/cmd_llm.bin（打包格式，格式字节 0 = fp32），可以直接
`tools/quantize_model.py --input assets/cmd_llm.bin --format q8` 压成 int8，
再 `tools/model.bat COM10 q8` 烧进 llm 分区。
"""
import argparse
import array
import json
import os
import struct
import sys

import torch
import torch.nn as nn
import torch.nn.functional as F

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from cmd_model_bpe import Tokenizer                    # noqa: E402
from quantize_model import fp32_tensors, parse_config, MAGIC   # noqa: E402

BOS = 1


# --------------------------------------------------------------------------- 模型
class CmdModel(nn.Module):
    """llama2 架构，和板子上跑的那份逐项对应（rmsnorm / RoPE / GQA / SwiGLU）。"""

    def __init__(self, cfg):
        super().__init__()
        self.cfg = cfg
        dim, hidden, L = cfg["dim"], cfg["hidden"], cfg["layers"]
        heads, kv_heads = cfg["heads"], cfg["kv_heads"]
        self.head_size = dim // heads
        self.kv_dim = dim * kv_heads // heads
        self.kv_mul = heads // kv_heads
        self.tok_emb = nn.Embedding(cfg["vocab"], dim)
        self.rms_att = nn.Parameter(torch.ones(L, dim))
        self.rms_ffn = nn.Parameter(torch.ones(L, dim))
        self.rms_final = nn.Parameter(torch.ones(dim))
        self.wq = nn.Parameter(torch.empty(L, dim, dim))
        self.wk = nn.Parameter(torch.empty(L, self.kv_dim, dim))
        self.wv = nn.Parameter(torch.empty(L, self.kv_dim, dim))
        self.wo = nn.Parameter(torch.empty(L, dim, dim))
        self.w1 = nn.Parameter(torch.empty(L, hidden, dim))
        self.w2 = nn.Parameter(torch.empty(L, dim, hidden))
        self.w3 = nn.Parameter(torch.empty(L, hidden, dim))
        for p in [self.wq, self.wk, self.wv, self.wo, self.w1, self.w2, self.w3]:
            nn.init.normal_(p, std=0.02)

    def rmsnorm(self, x, w):
        return x * torch.rsqrt(x.pow(2).mean(-1, keepdim=True) + 1e-5) * w

    def forward(self, idx):
        B, T = idx.shape
        cfg = self.cfg
        x = self.tok_emb(idx)
        # RoPE：与上游同样把 i%head_size 当作 head_dim，实时算（seq 很短，够快）
        i = torch.arange(0, cfg["dim"], 2, device=idx.device, dtype=torch.float32)
        head_dim = (i % self.head_size) / self.head_size
        freq = 1.0 / (10000.0 ** head_dim)
        pos = torch.arange(T, device=idx.device, dtype=torch.float32)
        ang = pos[:, None] * freq[None, :]                       # (T, dim/2)
        cos, sin = torch.cos(ang), torch.sin(ang)
        for l in range(cfg["layers"]):
            xb = self.rmsnorm(x, self.rms_att[l])
            q = F.linear(xb, self.wq[l])
            k = F.linear(xb, self.wk[l])
            v = F.linear(xb, self.wv[l])
            q = q.view(B, T, cfg["heads"], self.head_size)
            k = k.view(B, T, cfg["kv_heads"], self.head_size)
            v = v.view(B, T, cfg["kv_heads"], self.head_size)
            def rope(t, n_heads, width):
                # cos/sin 是 (T, dim/2)。k 只用到前面 kv_dim 个位置（上游
                # RoPE 里 `for i < kv_dim` 才转 k），所以先切宽度再按 head 分。
                # 形状必须是 (1, T, heads, hs/2)：第一个 1 让 batch 维广播，
                # 写成 (T, 1, ...) 会把 batch 和 T 相乘，得到一个假的 batch 维。
                c = cos[:, :width // 2].view(1, T, n_heads, self.head_size // 2)
                s = sin[:, :width // 2].view(1, T, n_heads, self.head_size // 2)
                # 与上游一致：按 (i, i+1) 成对旋转，i 的步长是 2
                t1, t2 = t[..., 0::2], t[..., 1::2]
                o1 = t1 * c - t2 * s
                o2 = t1 * s + t2 * c
                return torch.stack([o1, o2], dim=-1).flatten(-2)
            q = rope(q, cfg["heads"], cfg["dim"])
            k = rope(k, cfg["kv_heads"], self.kv_dim)
            # GQA：kv 头重复 kv_mul 次
            k = k.repeat_interleave(self.kv_mul, dim=2)
            v = v.repeat_interleave(self.kv_mul, dim=2)
            q = q.transpose(1, 2)                                # (B, heads, T, hs)
            k = k.transpose(1, 2)
            v = v.transpose(1, 2)
            att = (q @ k.transpose(-1, -2)) / (self.head_size ** 0.5)
            mask = torch.triu(torch.ones(T, T, device=idx.device, dtype=torch.bool), 1)
            att = att.masked_fill(mask, float("-inf"))
            att = F.softmax(att, dim=-1)
            if os.environ.get("CMD_DBG"):
                print(f"[dbg] L{l} q{tuple(q.shape)} k{tuple(k.shape)} v{tuple(v.shape)} "
                      f"att{tuple(att.shape)}")
            xb2 = (att @ v).transpose(1, 2).reshape(B, T, cfg["dim"])
            x = x + F.linear(xb2, self.wo[l])
            xb = self.rmsnorm(x, self.rms_ffn[l])
            hb = F.linear(xb, self.w1[l])
            hb2 = F.linear(xb, self.w3[l])
            hb = F.silu(hb) * hb2
            x = x + F.linear(hb, self.w2[l])
        x = self.rmsnorm(x, self.rms_final)
        # 分类器与嵌入共享（stories260K 就是共享的）
        return F.linear(x, self.tok_emb.weight)


# --------------------------------------------------------------------- 读/写权重
def load_checkpoint(path):
    """读上游 llama2.c 检查点（可能带我们的打包头）。"""
    raw = open(path, "rb").read()
    if len(raw) >= 16 and struct.unpack_from("<I", raw, 0)[0] == MAGIC:
        msize = struct.unpack_from("<I", raw, 4)[0]
        raw = raw[16:16 + msize]
    cfg = parse_config(raw)
    tensors, total = fp32_tensors(cfg)
    vals = torch.frombuffer(bytearray(raw[28:28 + total * 4]),
                            dtype=torch.float32).clone()
    tmap = {t["name"]: t for t in tensors}
    return cfg, vals, tmap, raw, total


def load_into(model, vals, tmap, cfg):
    L, D, H, V = cfg["layers"], cfg["dim"], cfg["hidden"], cfg["vocab"]
    kv = cfg["kv_dim"]
    def take(name, shape):
        t = tmap[name]
        return vals[t["off"]:t["off"] + t["count"]].view(*shape)
    with torch.no_grad():
        model.tok_emb.weight.copy_(take("token_embedding", (V, D)))
        model.rms_att.copy_(take("rms_att", (L, D)))
        model.rms_ffn.copy_(take("rms_ffn", (L, D)))
        model.rms_final.copy_(take("rms_final", (D,)))
        model.wq.copy_(take("wq", (L, D, D)))
        model.wk.copy_(take("wk", (L, kv, D)))
        model.wv.copy_(take("wv", (L, kv, D)))
        model.wo.copy_(take("wo", (L, D, D)))
        model.w1.copy_(take("w1", (L, H, D)))
        model.w2.copy_(take("w2", (L, D, H)))
        model.w3.copy_(take("w3", (L, H, D)))


def save_checkpoint(model, cfg, raw_orig, out_path, tok_blob):
    """写回上游 llama2.c 的 fp32 布局，外加我们 16 字节打包头。"""
    L, D, H, V = cfg["layers"], cfg["dim"], cfg["hidden"], cfg["vocab"]
    kv = cfg["kv_dim"]
    tensors, total = fp32_tensors(cfg)
    tmap = {t["name"]: t for t in tensors}
    out = array.array("f", bytes(total * 4))
    def put(name, t):
        tblk = tmap[name]
        flat = t.detach().reshape(-1).to(torch.float32).numpy()
        out[tblk["off"]:tblk["off"] + tblk["count"]] = array.array("f", flat.tobytes())
    with torch.no_grad():
        put("token_embedding", model.tok_emb.weight)
        put("rms_att", model.rms_att)
        put("rms_ffn", model.rms_ffn)
        put("rms_final", model.rms_final)
        put("wq", model.wq)
        put("wk", model.wk)
        put("wv", model.wv)
        put("wo", model.wo)
        put("w1", model.w1)
        put("w2", model.w2)
        put("w3", model.w3)
    if sys.byteorder != "little":
        out.byteswap()
    model_blob = raw_orig[:28] + out.tobytes()      # 28 字节 Config 原样保留（含共享分类器标志）
    with open(out_path, "wb") as f:
        f.write(struct.pack("<IIII", MAGIC, len(model_blob), len(tok_blob), 0))
        f.write(model_blob)
        f.write(tok_blob)
    return len(model_blob) + 16 + len(tok_blob)


# ------------------------------------------------------------------------ 训练
def make_seq(tok, cmd, target):
    prompt = tok.encode("cmd: " + cmd + "\n", bos=True, eos=False)
    body = tok.encode(target, bos=False, eos=False)
    ids = prompt + body + [BOS]
    # 只对"答案"算损失。注意因果错位：位置 t 的 logits 预测的是 ids[t+1]，
    # 所以 body 的第一个 token 由 prompt 的最后一个位置预测，标签要往前挪一格。
    labels = [-100] * (len(prompt) - 1) + body + [BOS] + [-100]
    assert len(labels) == len(ids), (len(labels), len(ids))
    return ids, labels


def batches(rows, tok, bs, rnd):
    idx = list(range(len(rows)))
    rnd.shuffle(idx)
    for i in range(0, len(idx), bs):
        chunk = [rows[j] for j in idx[i:i + bs]]
        seqs = [make_seq(tok, r["cmd"], r["json"]) for r in chunk]
        L = max(len(s[0]) for s in seqs)
        x = torch.zeros(len(seqs), L, dtype=torch.long)
        y = torch.full((len(seqs), L), -100, dtype=torch.long)
        for k, (ids, lab) in enumerate(seqs):
            x[k, :len(ids)] = torch.tensor(ids)
            y[k, :len(lab)] = torch.tensor(lab)
        yield x, y


def greedy(model, tok, cmd, max_new=40):
    ids = tok.encode("cmd: " + cmd + "\n", bos=True, eos=False)
    model.eval()
    with torch.no_grad():
        for _ in range(max_new):
            logits = model(torch.tensor(ids, dtype=torch.long)[None, :])
            nxt = int(logits[0, -1].argmax())
            if nxt == BOS:
                break
            ids.append(nxt)
    text = tok.decode_all(ids[len(tok.encode('cmd: ' + cmd + chr(10), bos=True, eos=False)):])
    return text


def score(model, tok, rows):
    exact = 0
    for r in rows:
        got = greedy(model, tok, r["cmd"]).strip()
        exact += 1 if got == r["json"] else 0
    return exact, len(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", default=os.path.expanduser("~/qc/cmd_data"))
    ap.add_argument("--init", default=os.path.join(ROOT, "assets", "stories260K.bin"))
    ap.add_argument("--tok", default=os.path.join(ROOT, "assets", "tok512.bin"))
    ap.add_argument("--out", default=os.path.join(ROOT, "assets", "cmd_llm.bin"))
    ap.add_argument("--steps", type=int, default=1500)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--eval-every", type=int, default=100)
    ap.add_argument("--seed", type=int, default=1337)
    ap.add_argument("--save", default=os.path.expanduser("~/qc/cmd_model.pt"),
                    help="torch 权重存档（便于续训/对比）")
    a = ap.parse_args()

    torch.manual_seed(a.seed)
    import random
    rnd = random.Random(a.seed)

    tok = Tokenizer(a.tok)
    cfg, vals, tmap, raw_orig, total = load_checkpoint(a.init)
    cfg["vocab"] = tok.vocab_size          # 与分词器一致（512）
    print(f"初始权重 {os.path.relpath(a.init, ROOT)}：{cfg['layers']} 层, dim {cfg['dim']}, "
          f"hidden {cfg['hidden']}, vocab {cfg['vocab']}")

    train = [json.loads(l) for l in open(os.path.join(a.data, "train.jsonl"), encoding="utf-8")]
    held = [json.loads(l) for l in open(os.path.join(a.data, "heldout.jsonl"), encoding="utf-8")]
    print(f"训练 {len(train)} / 留出 {len(held)}")

    model = CmdModel(cfg)
    load_into(model, vals, tmap, cfg)

    before = score(model, tok, held[:12])
    print(f"微调前（留出集前 12 条）：{before[0]}/{before[1]} 完全匹配")

    opt = torch.optim.AdamW(model.parameters(), lr=a.lr, weight_decay=0.01)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=a.lr, total_steps=a.steps,
                                                pct_start=0.05)
    best = (-1, None)
    step = 0
    model.train()
    while step < a.steps:
        for x, y in batches(train, tok, a.batch, rnd):
            if step >= a.steps:
                break
            logits = model(x)
            loss = F.cross_entropy(logits.reshape(-1, logits.shape[-1]),
                                   y.reshape(-1), ignore_index=-100)
            opt.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            sched.step()
            step += 1
            if step % a.eval_every == 0 or step == a.steps:
                ok, n = score(model, tok, held)
                flag = ""
                if ok > best[0]:
                    best = (ok, {k: v.clone() for k, v in model.state_dict().items()})
                    flag = "  <- 存"
                print(f"  step {step:>5}  loss {loss.item():.4f}  留出集 {ok}/{n}{flag}")
                model.train()

    if best[1] is not None:
        model.load_state_dict(best[1])
    ok, n = score(model, tok, held)
    print(f"最终（取留出集最好的一版）：留出集 {ok}/{n}，训练集 ", end="")
    ok2, n2 = score(model, tok, train)
    print(f"{ok2}/{n2}")
    torch.save({"cfg": cfg, "state": model.state_dict()}, a.save)
    size = save_checkpoint(model, cfg, raw_orig, a.out, open(a.tok, "rb").read())
    print(f"写出 {a.out}（{size:,} B，含打包头）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
