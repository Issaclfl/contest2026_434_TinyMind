#!/usr/bin/env python3
"""把我们自己的量化器写出来：fp32 检查点 -> int8 / int4 权重。

为什么要有这一步：llama2.c 的检查点是 fp32，0.26M 参数要 1.04 MB，而 S3 上
推理的瓶颈是**每生成一个 token 都要把所有权重再读一遍**。台架上量到的有效
带宽只有 ~24 MB/s，1.04 MB/token 就是 43 ms——正好对上实测的 21~26 tok/s。
把权重压成 int8 / int4，每 token 要读的字节数减到 1/4 或 1/8，速度就跟着上来。
这是实测结论，不是推测（见 docs/ESP32-S3网关台架实测证据.md）。

为什么不用 llama.cpp / llama2.c runq.c 的 group 方案：那套要求"组大小整除
每一个被量化的维度"，而 stories260K 的 hidden_dim = 172 不是 32/64 的倍数，
尾巴上会读到没量化的元素。我们改成**逐行（逐输出通道）对称量化**：

    一行 = 一个输出神经元的全部权重，共用一个 fp32 缩放因子
      s = max|x| / 127        (int8，q ∈ [-127, 127])
      s = max|x| / 7          (int4，q ∈ [-8, 7]，存成 q+8 塞进半字节)
      q = clamp(round(x / s))

矩阵乘：激活向量按整条量化成一个 int8 向量（一个标量缩放因子），
内积在 int32 上累加（127×127×512 = 8.3M，远在 int32 里），最后乘回
x_s * w_s[i]。逐行量化对 int8 来说几乎无损（每行 64~172 个权重共享一个
缩放因子），而且**没有任何维度整除约束**——这是选它的理由。

打包文件（就是我们 flash 进 llm 分区的那份）：
    偏移 0   u32 魔数 'LLM1'
    偏移 4   u32 模型字节数
    偏移 8   u32 分词器字节数
    偏移 12  u32 格式：0 = 上游 fp32（fetch_model.py 写的），1 = int8，2 = int4
    偏移 16  模型字节，紧跟分词器字节（分词器原样搬运）

模型字节内部（格式 1/2）：
    Config 28 B（与上游同布局，vocab_size 负数仍表示"分类器不共享"）
    fp32 rms_att_weight   (n_layers*dim)
    fp32 rms_ffn_weight   (n_layers*dim)
    fp32 rms_final_weight (dim)
    q    token_embedding  (vocab*dim*bits/8) + fp32 scales (vocab)
    每一层：wq wk wv wo w1 w2 w3，各 = q (rows*cols*bits/8) + fp32 scales (rows)
    分类器不共享时再来一份 wcls
    （上游 fp32 布局里那两段 freq_cis RoPE 表我们不写：移植后的引擎根本不读它）

行列按上游 memory_map_weights 的顺序排（层优先），所以 C 侧只要按下标算：
    第 layer 行区间起点 = q + (layer*rows)*row_bytes

用法：
    python3 tools/quantize_model.py --format q8            # -> assets/llm_q8.bin
    python3 tools/quantize_model.py --format q4            # -> assets/llm_q4.bin
    python3 tools/quantize_model.py --format q8 --report   # 再打一张逐张量表
"""
import argparse
import array
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASSETS = os.path.join(ROOT, "assets")

MAGIC = 0x314D4C4C          # 'LLM1'
FMT_FP32, FMT_INT8, FMT_INT4 = 0, 1, 2
FORMATS = {"q8": FMT_INT8, "q4": FMT_INT4}
BITS = {FMT_INT8: 8, FMT_INT4: 4}

# 量化张量的书写顺序，必须与 components/espllm/espllm_engine.c 的映射一致。
PER_LAYER = ("wq", "wk", "wv", "wo", "w1", "w2", "w3")


def load_pack(path):
    with open(path, "rb") as f:
        head = f.read(16)
        if len(head) != 16:
            raise SystemExit(f"{path} 太小，读不到 16 字节打包头")
        magic, msize, tsize, fmt = struct.unpack("<IIII", head)
        if magic != MAGIC:
            raise SystemExit(f"{path} 的魔数是 0x{magic:08x}，不是打包文件")
        model = f.read(msize)
        tok = f.read(tsize)
    if len(model) != msize or len(tok) != tsize:
        raise SystemExit(f"{path} 被截断：头里写着 {msize}+{tsize}，实际读到 "
                         f"{len(model)}+{len(tok)}")
    return model, tok, fmt


def parse_config(model):
    """上游检查点开头就是 7 个 int。vocab_size 为负 = 分类器不共享嵌入表。"""
    dim, hidden, layers, heads, kv_heads, vocab, seq_len = struct.unpack_from("<7i", model, 0)
    if vocab == 0 or dim <= 0 or hidden <= 0 or layers <= 0 or heads <= 0 or kv_heads <= 0:
        raise SystemExit(f"模型头看着不对：{dim} {hidden} {layers} {heads} {kv_heads} {vocab} {seq_len}")
    return {
        "dim": dim, "hidden": hidden, "layers": layers, "heads": heads,
        "kv_heads": kv_heads, "vocab": abs(vocab), "seq_len": seq_len,
        "shared": vocab > 0,
        "kv_dim": dim * kv_heads // heads,
    }


def fp32_tensors(cfg):
    """上游 memory_map_weights 的顺序与偏移（单位：float）。quant=False 的保持 fp32。"""
    L, D, H, V = cfg["layers"], cfg["dim"], cfg["hidden"], cfg["vocab"]
    KV = cfg["kv_dim"]
    head = D // cfg["heads"]
    out, off = [], 0

    def add(name, count, rows, cols, quant=True):
        nonlocal off
        out.append({"name": name, "off": off, "count": count,
                    "rows": rows, "cols": cols, "quant": quant})
        off += count

    add("token_embedding", V * D, V, D)
    add("rms_att", L * D, L, D, quant=False)
    add("wq", L * D * D, L * D, D)
    add("wk", L * D * KV, L * KV, D)
    add("wv", L * D * KV, L * KV, D)
    add("wo", L * D * D, L * D, D)
    add("rms_ffn", L * D, L, D, quant=False)
    add("w1", L * D * H, L * H, D)
    add("w2", L * H * D, L * D, H)
    add("w3", L * D * H, L * H, D)
    add("rms_final", D, 1, D, quant=False)
    off += cfg["seq_len"] * head          # freq_cis_real / imag：上游留着，我们不读
    if not cfg["shared"]:
        add("wcls", V * D, V, D)
    return out, off


def quant_size(cfg, fmt, counts):
    """按 C 侧映射公式算出的模型字节数——与写入量对不上就说明两边布局有分歧。"""
    bits = BITS[fmt]
    L, D = cfg["layers"], cfg["dim"]
    n = 4 * (2 * L * D + D)                       # 三段 fp32 的 rms 权重（att / ffn / final）
    names = ("token_embedding",) + PER_LAYER + (() if cfg["shared"] else ("wcls",))
    for name in names:
        rows, cols = counts[name]
        n += rows * cols * bits // 8 + 4 * rows
    return n


def quantize_tensor(vals, off, rows, cols, bits, name, stats):
    """逐行对称量化。返回 (打包好的权重字节, 缩放因子 array)。"""
    qmax = 127 if bits == 8 else 7
    packed = bytearray()
    scales = array.array("f")
    err2 = 0.0
    sig2 = 0.0
    worst = 0.0
    for r in range(rows):
        base = off + r * cols
        amax = 0.0
        for j in range(cols):
            a = abs(vals[base + j])
            if a > amax:
                amax = a
        s = (amax / qmax) if amax > 0.0 else 0.0
        scales.append(s)
        inv = (1.0 / s) if s > 0.0 else 0.0
        if bits == 8:
            for j in range(cols):
                x = vals[base + j]
                q = int(round(x * inv)) if inv else 0
                q = 127 if q > 127 else (-127 if q < -127 else q)
                packed.append(q & 0xFF)
                d = x - q * s
                err2 += d * d
                sig2 += x * x
                if abs(d) > worst:
                    worst = abs(d)
        else:
            for j in range(0, cols, 2):
                x0 = vals[base + j]
                x1 = vals[base + j + 1]
                q0 = int(round(x0 * inv)) if inv else 0
                q1 = int(round(x1 * inv)) if inv else 0
                q0 = 7 if q0 > 7 else (-8 if q0 < -8 else q0)
                q1 = 7 if q1 > 7 else (-8 if q1 < -8 else q1)
                packed.append(((q0 + 8) & 0xF) | (((q1 + 8) & 0xF) << 4))
                d0 = x0 - q0 * s
                d1 = x1 - q1 * s
                err2 += d0 * d0 + d1 * d1
                sig2 += x0 * x0 + x1 * x1
                for d in (d0, d1):
                    if abs(d) > worst:
                        worst = abs(d)
    stats[name] = {
        "rows": rows, "cols": cols,
        "rel_rms": (err2 / sig2) ** 0.5 if sig2 > 0 else 0.0,
        "abs_max": worst,
    }
    return bytes(packed), scales


def dequantize_pack(in_path, out_path):
    """把量化包还原成 fp32 包（格式 0）。

    这一步是为了把两种误差分开：量化本身的误差（权重被压成 int8/int4），和
    int8 内核的误差（点积实现得对不对）。还原出来的包走的是**已经验证过的
    fp32 内核**，权重却和量化包完全一样——两个包跑出来的文本如果基本一致，
    就说明 int8 内核是忠实的；不一致，问题在实现而不是量化。
    """
    model, tok, fmt = load_pack(in_path)
    if fmt == FMT_FP32:
        raise SystemExit(f"{in_path} 已经是 fp32 了")
    bits = BITS[fmt]
    cfg = parse_config(model)
    L, D, H, V = cfg["layers"], cfg["dim"], cfg["hidden"], cfg["vocab"]
    KV = cfg["kv_dim"]
    tensors, total_floats = fp32_tensors(cfg)
    tmap = {t["name"]: t for t in tensors}

    off = 28
    rms_bytes = 4 * (2 * L * D + D)
    rms_block = model[off:off + rms_bytes]
    off += rms_bytes
    out = bytearray(total_floats * 4)

    def place(name, blob, count):
        """把 count 个 float 写到张量 name 在上游 fp32 布局里的位置。
        必须写成 out[o:o+n] —— 写成 out[o:][:n] 会落到切片副本上，包是空的。"""
        o = tmap[name]["off"] * 4
        out[o:o + count * 4] = blob

    place("rms_att", rms_block[:L * D * 4], L * D)
    place("rms_ffn", rms_block[L * D * 4:2 * L * D * 4], L * D)
    place("rms_final", rms_block[2 * L * D * 4:], D)

    names = ["token_embedding"] + list(PER_LAYER)
    if not cfg["shared"]:
        names.append("wcls")
    for name in names:
        t = tmap[name]
        rows, cols = t["rows"], t["cols"]
        qbytes = rows * cols * bits // 8
        q = model[off:off + qbytes]
        off += qbytes
        sc = array.array("f")
        sc.frombytes(model[off:off + 4 * rows])
        off += 4 * rows
        vals = array.array("f", bytes(rows * cols * 4))
        for r in range(rows):
            s = sc[r]
            base = r * cols
            if bits == 8:
                for j in range(cols):
                    b = q[base + j]
                    vals[base + j] = ((b - 256) if b > 127 else b) * s
            else:
                for j in range(0, cols, 2):
                    b = q[(base + j) // 2]
                    vals[base + j] = ((b & 0x0F) - 8) * s
                    vals[base + j + 1] = ((b >> 4) - 8) * s
        if sys.byteorder != "little":
            vals.byteswap()
        place(name, vals.tobytes(), rows * cols)
    if off != len(model):
        raise SystemExit(f"还原时读了 {off} B，模型是 {len(model)} B——布局对不上")

    model_out = model[:28] + bytes(out)
    with open(out_path, "wb") as f:
        f.write(struct.pack("<IIII", MAGIC, len(model_out), len(tok), FMT_FP32))
        f.write(model_out)
        f.write(tok)
    print(f"还原成 fp32 包 {out_path}（{len(model_out):,} B，权重与量化包一致）")
    return out_path


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--format", default="q8", choices=sorted(FORMATS))
    ap.add_argument("--input", default=os.path.join(ASSETS, "llm.bin"))
    ap.add_argument("--output", default=None)
    ap.add_argument("--report", action="store_true", help="逐张量打印误差")
    ap.add_argument("--roundtrip", default=None, metavar="PATH",
                    help="再把量化包还原成 fp32 包写到 PATH（用来分开量化误差与内核误差）")
    args = ap.parse_args()

    fmt = FORMATS[args.format]
    bits = BITS[fmt]
    out_path = args.output or os.path.join(ASSETS, f"llm_{args.format}.bin")

    model, tok, in_fmt = load_pack(args.input)
    cfg = parse_config(model)
    tensors, total_floats = fp32_tensors(cfg)

    have = (len(model) - 28) // 4
    if have != total_floats:
        raise SystemExit(f"偏移表对不上：模型里 {have} 个 float，表算出 {total_floats} 个。"
                         " 上游布局变了？")

    print(f"输入  {args.input}")
    print(f"      {len(model):,} B 模型（格式 {in_fmt}）+ {len(tok):,} B 分词器，"
          f"{sum(t['count'] for t in tensors if t['quant']):,} 个待量化参数")
    print(f"      {cfg['layers']} 层，dim {cfg['dim']}，hidden {cfg['hidden']}，"
          f"vocab {cfg['vocab']}，{'共享' if cfg['shared'] else '不共享'}分类器")

    vals = array.array("f")
    vals.frombytes(model[28:28 + total_floats * 4])
    if sys.byteorder != "little":
        vals.byteswap()

    stats = {}
    counts = {}
    body = bytearray()
    # 1) 三段 fp32 的 rmsnorm 权重先写（它们本来就不量化）
    for t in tensors:
        if not t["quant"]:
            body += model[28 + t["off"] * 4: 28 + (t["off"] + t["count"]) * 4]
    # 2) 量化张量
    for t in tensors:
        if not t["quant"]:
            continue
        if t["name"] in PER_LAYER or t["name"] in ("token_embedding", "wcls"):
            name = t["name"]
            if t["cols"] % 2 != 0 and bits == 4:
                raise SystemExit(f"{name} 的列数 {t['cols']} 是奇数，int4 按行打包会串位")
            packed, scales = quantize_tensor(vals, t["off"], t["rows"], t["cols"],
                                             bits, name, stats)
            counts[name] = (t["rows"], t["cols"])
            body += packed
            if sys.byteorder != "little":
                scales.byteswap()
            body += scales.tobytes()
            print(f"  量化 {name:<16} {t['rows']:>5} 行 × {t['cols']:>4} 列"
                  f"  {len(packed) + 4 * t['rows']:>9,} B")

    expect = quant_size(cfg, fmt, counts)
    if len(body) != expect:
        raise SystemExit(f"写入 {len(body):,} B，按 C 侧映射公式应为 {expect:,} B"
                         "——布局公式两边不一致，先别烧")
    print(f"      C 侧映射公式核对：{len(body):,} B ✓")

    model_out = model[:28] + bytes(body)
    with open(out_path, "wb") as f:
        f.write(struct.pack("<IIII", MAGIC, len(model_out), len(tok), fmt))
        f.write(model_out)
        f.write(tok)

    ratio = len(model) / len(model_out)
    print(f"输出  {out_path}")
    print(f"      模型 {len(model):,} B -> {len(model_out):,} B（{ratio:.2f}×，"
          f"格式 {'int8' if bits == 8 else 'int4'}）"
          f"，整包 {16 + len(model_out) + len(tok):,} B")
    if args.report:
        print()
        print(f"      {'张量':<16}{'行×列':>14}{'相对 RMS 误差':>16}{'最大绝对误差':>16}")
        for name, s in stats.items():
            dims = "%d×%d" % (s["rows"], s["cols"])
            print(f"      {name:<16}{dims:>14}{s['rel_rms']:>16.5f}{s['abs_max']:>16.5f}")
    else:
        worst = max(stats.items(), key=lambda kv: kv[1]["rel_rms"])
        print(f"      最差张量 {worst[0]}：相对 RMS 误差 {worst[1]['rel_rms']:.5f}"
              "（--report 看全部）")
    print()
    print(f"下一步：tools\\model.bat COMx  拷 {os.path.basename(out_path)} 到 llm 分区"
          "（或先 cp 成 assets/llm.bin 再烧）")
    if args.roundtrip:
        print()
        dequantize_pack(out_path, args.roundtrip)
    return 0


if __name__ == "__main__":
    sys.exit(main())
