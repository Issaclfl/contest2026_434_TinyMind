#!/usr/bin/env python3
"""量化到底损失了多少：同一段前缀下，不同权重格式给出的下一个 token 是否一致。

为什么不用"生成结果像不像"来比：贪心解码一旦某个位置的 argmax 被翻掉，后面
就是两条完全不同的句子，字符前缀长度只能说明"第一次翻转发生得多早"，不能说明
模型本身差多少。这里改成**逐位置比对**：拿 fp32 生成一段参考文本，然后取它的
一段段前缀当 prompt，问每个格式"下一个 token 是什么"，直接数一致率。前缀越长
上下文越完整，这个数字比前缀长度稳得多。

用法（先用 host_check 在 PC 上跑，见 tools/host_check.c 顶部的编译命令）：
    python3 tools/quant_agreement.py ~/qc/host_check \\
        assets/llm.bin assets/llm_q8.bin assets/llm_q4.bin
    python3 tools/quant_agreement.py ~/qc/host_check assets/llm.bin assets/llm_q4.bin \\
        --prompt "Once upon a time" --steps 120 --points 24
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASSETS = os.path.join(ROOT, "assets")


def next_token(binary, pack, prompt):
    """让某个权重格式回答"下一个 token"，返回它的文本（去掉换行）。"""
    out = subprocess.run([binary, pack, prompt, "1", "0"],
                         capture_output=True, cwd=ROOT)
    if out.returncode != 0:
        raise SystemExit(f"{pack} 跑不起来：{out.stderr.decode('utf-8', 'replace')[:200]}")
    return out.stdout.decode("utf-8", "replace").strip("\n")


def generate(binary, pack, prompt, steps):
    out = subprocess.run([binary, pack, prompt, str(steps), "0"],
                         capture_output=True, cwd=ROOT)
    if out.returncode != 0:
        raise SystemExit(f"{pack} 跑不起来：{out.stderr.decode('utf-8', 'replace')[:200]}")
    return out.stdout.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary", help="tools/host_check.c 编出来的可执行文件")
    ap.add_argument("packs", nargs="+", help="第一个是参考（通常是 fp32），后面是待比对")
    ap.add_argument("--prompt", default="Once upon a time, there was a little robot")
    ap.add_argument("--steps", type=int, default=200, help="参考文本生成多少 token")
    ap.add_argument("--points", type=int, default=20, help="取多少个前缀位置比对")
    args = ap.parse_args()

    ref_pack = args.packs[0]
    print(f"参考 {ref_pack} 生成 {args.steps} token …")
    ref = generate(args.binary, ref_pack, args.prompt, args.steps)
    print(f"  参考文本 {len(ref)} 字符：{ref[:90].replace(chr(10), ' ')} …")

    base = args.prompt + ref
    n = len(base)
    # 从 1/4 处开始取点：太短的前缀上下文不足，比出来的东西不稳定
    points = [int(n * (i + 1) / (args.points + 1)) for i in range(args.points)]
    points = [p for p in points if p > len(args.prompt)]

    print()
    print(f"{'前缀字符数':>10}  " + "  ".join(f"{os.path.basename(p):>14}" for p in args.packs[1:]))
    hits = {p: 0 for p in args.packs[1:]}
    total = 0
    for off in points:
        prefix = base[:off]
        want = next_token(args.binary, ref_pack, prefix)
        row = []
        for p in args.packs[1:]:
            got = next_token(args.binary, p, prefix)
            ok = got == want
            hits[p] += 1 if ok else 0
            row.append(f"{'一致' if ok else '不一致':>14}")
        total += 1
        print(f"{off:>10}  " + "  ".join(row))

    print()
    for p in args.packs[1:]:
        print(f"{os.path.basename(p):>22}: 与参考一致 {hits[p]}/{total}"
              f"（{100.0 * hits[p] / max(total, 1):.0f}%）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
