#!/usr/bin/env python3
"""把云端 LLM 的 API key 写进 sdkconfig，不用进 menuconfig 的界面。

与 set_wifi.py 同一套规矩：凭据只落在 sdkconfig（被 .gitignore 排除）里，
不进命令行历史、聊天记录或仓库。key 写进去之后固件会把上行在线的请求代理
到云端大模型，断网或云端失败时自动改由板载本地模型作答。

用法：
    python tools/set_cloud.py                 # 交互式问 key（可选覆盖 URL/模型名）
    python tools/set_cloud.py --show          # 只看现在配了什么（key 打码）
    python tools/set_cloud.py --clear         # 清空 key
"""
import argparse
import getpass
import os
import re
import sys

SDKCONFIG = os.path.join(os.getcwd(), "sdkconfig")
KEY = "CONFIG_GATEWAY_CLOUD_API_KEY"
URL = "CONFIG_GATEWAY_CLOUD_URL"
MODEL = "CONFIG_GATEWAY_CLOUD_MODEL"


def sdkconfig_escape(value):
    """sdkconfig 里字符串是带引号的 C 字符串，反斜杠与引号要转义。"""
    return value.replace("\\", "\\\\").replace('"', '\\"')


def read_current():
    if not os.path.isfile(SDKCONFIG):
        return None, None, None
    url = model = key = None
    for line in open(SDKCONFIG, encoding="utf-8", errors="replace"):
        m = re.match(rf'{URL}="(.*)"\s*$', line)
        if m:
            url = m.group(1)
        m = re.match(rf'{MODEL}="(.*)"\s*$', line)
        if m:
            model = m.group(1)
        m = re.match(rf'{KEY}="(.*)"\s*$', line)
        if m:
            key = m.group(1)
    return url, model, key


def write_line(lines_map):
    lines = open(SDKCONFIG, encoding="utf-8", errors="replace").read().splitlines(keepends=True)
    out = []
    seen = {k: False for k in lines_map}
    for line in lines:
        hit = False
        for k, v in lines_map.items():
            if line.startswith(k):
                out.append(f'{k}="{sdkconfig_escape(v)}"\n')
                seen[k] = True
                hit = True
                break
        if not hit:
            out.append(line)
    for k, v in lines_map.items():
        if not seen[k]:
            out.append(f'{k}="{sdkconfig_escape(v)}"\n')
    with open(SDKCONFIG, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--show", action="store_true", help="显示当前配置（key 打码）")
    ap.add_argument("--clear", action="store_true", help="清空 key")
    args = ap.parse_args()

    if not os.path.isfile(SDKCONFIG):
        raise SystemExit(
            f"找不到 {SDKCONFIG}\n"
            "先在工程目录里跑一次构建（tools\\build.bat），让 sdkconfig 生成出来。")

    if args.show:
        url, model, key = read_current()
        print(f"  URL    : {url or '（用 Kconfig 默认值）'}")
        print(f"  model  : {model or '（用 Kconfig 默认值）'}")
        print(f"  api key: {'（已设置，' + str(len(key)) + ' 字符）' if key else '（空）'}")
        return 0

    if args.clear:
        write_line({KEY: ""})
        print("key 已清空；所有请求将由本地模型回答。重新构建后生效。")
        return 0

    print("把云端 LLM 的 API key 写进 sdkconfig（只落在这个文件里，不进仓库）。")
    print("key 输入时不回显，也不会被打印出来。")
    key = getpass.getpass("API key: ")
    if not key:
        raise SystemExit("key 不能为空")

    lines = {KEY: key}
    url = input("URL（回车用默认 https://token-plan-cn.xiaomimimo.com/v1/chat/completions）: ").strip()
    if url:
        lines[URL] = url
    model = input("模型名（回车用默认 mimo-v2.5-pro；该服务端一度对 -pro 回 500，"
                  "实测 mimo-v2.5 可用）: ").strip()
    if model:
        lines[MODEL] = model

    write_line(lines)
    print(f"已写入：key {len(key)} 字符"
          + (f"，URL {url[:24]}…" if url else "")
          + (f"，model {model}" if model else "")
          + "。")
    print()
    print("下一步：")
    print("    tools\\build.bat")
    print("    tools\\flash.bat COMx")
    return 0


if __name__ == "__main__":
    sys.exit(main())
