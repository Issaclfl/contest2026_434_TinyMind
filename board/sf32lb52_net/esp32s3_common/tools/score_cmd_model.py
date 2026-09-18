#!/usr/bin/env python3
"""用板子上跑的那份引擎（编到 PC 上的 host_check）给命令模型打分。

为什么要用 C 引擎打分，而不是用训练时的 torch 模型打分：训练是 torch 做的，
板子上跑的是 C。两边只要有一点点不一致（分词、RoPE、GQA、停止条件），"torch 上
93%" 就是一句空话。所以覆盖率的数字必须来自**同一份引擎源码**。

用法：
    gcc -O2 -o ~/qc/host_check tools/host_check.c components/espllm/espllm_engine.c \
        -Icomponents/espllm/include -lm
    python3 tools/score_cmd_model.py --bin ~/qc/host_check --pack assets/cmd_llm.bin \
        --data ~/qc/cmd_data/heldout.jsonl [--show-fail 8]
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def run(binary, pack, cmd, max_new):
    """跑一次并取出"答案"部分。

    引擎会把 prompt 也 emit 出来（上游 generate() 的行为：走 prompt 时就打印），
    所以输出是 "cmd: <口令>\\n" + 答案；比较前要把它切掉。而 max_new 是**含 prompt
    在内**的 token 预算，太小会把答案截断，所以默认给宽一点。
    """
    prompt = "cmd: " + cmd + "\n"
    out = subprocess.run([binary, pack, prompt, str(max_new), "0"],
                         capture_output=True, cwd=ROOT)
    if out.returncode != 0:
        raise SystemExit(f"{pack} 跑不起来：{out.stderr.decode('utf-8', 'replace')[:200]}")
    text = out.stdout.decode("utf-8", "replace")
    if text.startswith(prompt):
        text = text[len(prompt):]
    return text.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, help="host_check 可执行文件")
    ap.add_argument("--pack", required=True, help="命令模型的包（含 16 字节打包头）")
    ap.add_argument("--data", required=True, help="jsonl，每行 {cmd, json}")
    ap.add_argument("--max-new", type=int, default=40,
                    help="token 预算（含 prompt，见 run() 的说明）")
    ap.add_argument("--show-fail", type=int, default=6)
    a = ap.parse_args()

    rows = [json.loads(l) for l in open(a.data, encoding="utf-8")]
    ok, fails = 0, []
    for r in rows:
        got = run(a.bin, a.pack, r["cmd"], a.max_new)
        if got == r["json"]:
            ok += 1
        else:
            fails.append((r["cmd"], r["json"], got))
    print(f"{os.path.basename(a.pack)}: 留出集完全匹配 {ok}/{len(rows)}"
          f"（{100.0 * ok / max(len(rows), 1):.1f}%）")
    for cmd, want, got in fails[:a.show_fail]:
        print(f"  错  {cmd!r}\n      想 {want}\n      得 {got!r}")
    return 0 if ok == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
