#!/usr/bin/env python3
"""把 llama2.c 检查点取下来，打包成固件要的一份文件。

为什么要有这一步：模型和分词器是两个独立文件，而分区里必须能看出"哪一段
是模型、哪一段是分词器"。如果按分区大小来推断，分区被扇区对齐过，多出来的
填充会被一起读进来（1 MB 的模型配 6 MB 的分区，就有 5 MB 是垃圾）——所以
打包时在开头写明两个长度，设备侧照着读，自描述、不靠猜。

    偏移 0   u32 魔数 'LLM1'
    偏移 4   u32 模型字节数
    偏移 8   u32 分词器字节数
    偏移 16  模型字节，紧跟分词器字节

产物是 assets/llm.bin，用 tools/flash_model.py 写进 flash 的 llm 分区。

用法：
    python tools/fetch_model.py                 # 默认 stories260K
    python tools/fetch_model.py --list          # 看有哪些可选
    python tools/fetch_model.py --model stories15M
    python tools/fetch_model.py --base https://huggingface.co   # 换源

huggingface.co 在境内直连不通，默认走 hf-mirror.com（同一套 API）。
"""
import argparse
import os
import struct
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASSETS = os.path.join(ROOT, "assets")

DEFAULT_BASE = "https://hf-mirror.com"

# (仓库, 模型文件, 分词器文件)。都是 MIT 许可的 llama2.c 检查点。
MODELS = {
    "stories260K": (
        "karpathy/tinyllamas",
        "stories260K/stories260K.bin",
        "stories260K/tok512.bin",
    ),
    "stories15M": (
        "karpathy/tinyllamas",
        "stories15M.bin",
        "tokenizer.bin",            # 在 karpathy/llama2.c 里，见 SPECIAL_BASE
    ),
    "stories42M": (
        "karpathy/tinyllamas",
        "stories42M.bin",
        "tokenizer.bin",
    ),
}

# 少数文件不在模型仓里：15M/42M 用的分词器在 llama2.c 代码仓。
SPECIAL_BASE = {
    "tokenizer.bin": "https://raw.githubusercontent.com/karpathy/llama2.c/master",
    # raw.githubusercontent 会抖，备用走 API（base64），见 fetch()。
    "tokenizer.bin_api": "https://api.github.com/repos/karpathy/llama2.c/contents/tokenizer.bin",
}

MAGIC = 0x314D4C4C   # 'LLM1'


def fetch(url, dest):
    """下载到 dest；已存在且非空就跳过（除非 --force）。"""
    print(f"  取 {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "tinyllama-fetch/1.0"})
    with urllib.request.urlopen(req, timeout=180) as r, open(dest, "wb") as f:
        total = 0
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
            total += len(chunk)
    print(f"    -> {dest}  {total:,} B")
    return dest


def fetch_llama2_file(name, dest):
    """从 llama2.c 代码仓取一个文件：先试 raw，失败退回 API(base64)。"""
    import base64
    import json

    raw = SPECIAL_BASE["tokenizer.bin"] + "/" + name
    try:
        return fetch(raw, dest)
    except Exception as exc:                      # noqa: BLE001
        print(f"    raw 取失败（{exc}），改用 API")
    api = SPECIAL_BASE["tokenizer.bin_api"]
    with urllib.request.urlopen(urllib.request.Request(
            api, headers={"User-Agent": "tinyllama-fetch/1.0"}), timeout=120) as r:
        payload = json.load(r)
    data = base64.b64decode(payload["content"])
    with open(dest, "wb") as f:
        f.write(data)
    print(f"    -> {dest}  {len(data):,} B (API)")
    return dest


def pack(model_path, tok_path, out_path):
    model = open(model_path, "rb").read()
    tok = open(tok_path, "rb").read()
    header = struct.pack("<III", MAGIC, len(model), len(tok)) + b"\x00" * 4
    with open(out_path, "wb") as f:
        f.write(header)
        f.write(model)
        f.write(tok)
    print(f"打包完成 {out_path}")
    print(f"  头部 16 B + 模型 {len(model):,} B + 分词器 {len(tok):,} B"
          f" = {16 + len(model) + len(tok):,} B")
    return out_path


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", default="stories260K", choices=sorted(MODELS))
    ap.add_argument("--base", default=DEFAULT_BASE, help="HF 镜像地址")
    ap.add_argument("--force", action="store_true", help="已下载也重取")
    ap.add_argument("--list", action="store_true", help="列出可选的模型后退出")
    args = ap.parse_args()

    if args.list:
        for name, (repo, m, t) in sorted(MODELS.items()):
            print(f"  {name:<12} {repo}  {m}  +  {t}")
        return 0

    repo, model_file, tok_file = MODELS[args.model]
    os.makedirs(ASSETS, exist_ok=True)
    model_path = os.path.join(ASSETS, os.path.basename(model_file))
    tok_path = os.path.join(ASSETS, os.path.basename(tok_file))
    out_path = os.path.join(ASSETS, "llm.bin")

    print(f"模型 {args.model}（{repo}，源 {args.base}）")
    if args.force or not os.path.isfile(model_path) or os.path.getsize(model_path) == 0:
        fetch(f"{args.base}/{repo}/resolve/main/{model_file}", model_path)
    else:
        print(f"  已有 {model_path}，跳过（--force 可重取）")

    if args.force or not os.path.isfile(tok_path) or os.path.getsize(tok_path) == 0:
        if tok_file == "tokenizer.bin":
            fetch_llama2_file(tok_file, tok_path)
        else:
            fetch(f"{args.base}/{repo}/resolve/main/{tok_file}", tok_path)
    else:
        print(f"  已有 {tok_path}，跳过（--force 可重取）")

    pack(model_path, tok_path, out_path)
    print()
    print("下一步：tools\\flash_model.py COMx   （写进 llm 分区）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
