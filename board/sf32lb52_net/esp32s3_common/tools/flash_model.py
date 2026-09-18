#!/usr/bin/env python3
"""把 assets/llm.bin 写进 flash 的 llm 分区。

偏移不写死：从 partitions.csv 里算出来（前面几行的大小累加、按 0x1000 对齐），
这样以后改分区表、换更大的模型，这个脚本不用跟着改。

用法：
    python tools/flash_model.py COM10
    python tools/flash_model.py COM10 --baud 921600
    python tools/flash_model.py COM10 --image assets/llm_q8.bin   # 量化后的模型
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

PART_CSV_DEFAULT = os.path.join(os.getcwd(), "partitions.csv")
IMAGE = os.path.join(ROOT, "assets", "llm.bin")
PART_NAME = "llm"
ALIGN = 0x1000


def partition_offset(csv_path, name):
    """算出一个分区在 flash 里的绝对偏移。

    规则与 ESP-IDF 一致：第一个分区从 0x9000 起（前面是 bootloader 与分区表），
    每个分区按 0x1000 对齐；表里写了偏移就用表里的值。
    """
    offset = 0x9000
    for line in open(csv_path, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split(",")]
        if len(parts) < 5:
            continue
        entry, _type, _subtype, size = parts[0], parts[1], parts[2], parts[4]
        this_name = entry
        explicit = parts[3] if len(parts) > 3 else ""
        if explicit:
            offset = int(explicit, 0)
        if this_name == name:
            return offset
        offset = (offset + int(size, 0) + ALIGN - 1) // ALIGN * ALIGN
    raise SystemExit(f"partition '{name}' not found in {csv_path}")


def esptool_python():
    """优先用 IDF 自带解释器（它一定装了 esptool）。"""
    idf_tools = os.environ.get("IDF_TOOLS_PATH", r"E:\Espressif")
    for candidate in (
        os.path.join(idf_tools, "python_env", "idf5.5_py3.11_env", "Scripts", "python.exe"),
        os.path.join(idf_tools, "tools", "idf-python", "3.11.2", "python.exe"),
    ):
        if os.path.isfile(candidate):
            return candidate
    return sys.executable


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="例如 COM10")
    ap.add_argument("--partitions", default=PART_CSV_DEFAULT,
                    help="分区表 CSV（默认当前目录的 partitions.csv）")
    ap.add_argument("--image", default=IMAGE,
                    help="要写进去的包（默认 assets/llm.bin；量化模型传 assets/llm_q8.bin）")
    ap.add_argument("--baud", type=int, default=921600)
    args = ap.parse_args()

    image = args.image
    if not os.path.isabs(image):
        # 相对路径优先按 esp32s3_common/ 解释（assets/ 在那儿），其次才是当前目录
        in_root = os.path.join(ROOT, image)
        image = in_root if os.path.isfile(in_root) else os.path.join(os.getcwd(), image)
    if not os.path.isfile(image):
        raise SystemExit(f"missing {image}\n先跑： python tools/fetch_model.py")

    offset = partition_offset(args.partitions, PART_NAME)
    size = os.path.getsize(image)
    print(f"{image}\n  {size:,} B -> {PART_NAME} 分区 @ 0x{offset:06x} ({args.port}, {args.baud} baud)")

    cmd = [esptool_python(), "-m", "esptool",
           "--chip", "esp32s3", "--port", args.port, "--baud", str(args.baud),
           "--before", "default_reset", "--after", "hard_reset",
           "write_flash", hex(offset), image]
    print("  " + " ".join(cmd))
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
