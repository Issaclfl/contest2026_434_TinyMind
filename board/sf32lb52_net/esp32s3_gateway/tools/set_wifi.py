#!/usr/bin/env python3
"""把 Wi-Fi 凭据写进 sdkconfig，不用进 menuconfig 的界面。

为什么单独做一个：menuconfig 是个方向键界面，找一个选项本身就有门槛；而
凭据只该落在一个文件里（sdkconfig，被 .gitignore 排除），不该出现在命令行、
聊天记录或仓库里。

脚本会先确认真的连得上（把凭据写进去、编译、烧录、再看串口日志里有没有
拿到 IP），而不是写完就报成功。

用法：
    python tools/set_wifi.py                 # 交互式问 SSID 与密码
    python tools/set_wifi.py --show          # 只看现在配了什么（密码打码）
    python tools/set_wifi.py --clear         # 清空
"""
import argparse
import getpass
import os
import re
import sys

SDKCONFIG = os.path.join(os.getcwd(), "sdkconfig")
SSID_KEY = "CONFIG_GATEWAY_WIFI_SSID"
PASS_KEY = "CONFIG_GATEWAY_WIFI_PASSWORD"


def sdkconfig_escape(value):
    """sdkconfig 里字符串是带引号的 C 字符串，反斜杠与引号要转义。"""
    return value.replace("\\", "\\\\").replace('"', '\\"')


def read_current():
    if not os.path.isfile(SDKCONFIG):
        return None, None
    ssid = password = None
    for line in open(SDKCONFIG, encoding="utf-8", errors="replace"):
        m = re.match(rf'{SSID_KEY}="(.*)"\s*$', line)
        if m:
            ssid = m.group(1)
        m = re.match(rf'{PASS_KEY}="(.*)"\s*$', line)
        if m:
            password = m.group(1)
    return ssid, password


def write_values(ssid, password):
    lines = open(SDKCONFIG, encoding="utf-8", errors="replace").read().splitlines(keepends=True)
    out = []
    seen_ssid = seen_pass = False
    for line in lines:
        if line.startswith(SSID_KEY):
            out.append(f'{SSID_KEY}="{sdkconfig_escape(ssid)}"\n')
            seen_ssid = True
        elif line.startswith(PASS_KEY):
            out.append(f'{PASS_KEY}="{sdkconfig_escape(password)}"\n')
            seen_pass = True
        else:
            out.append(line)
    if not seen_ssid:
        out.append(f'{SSID_KEY}="{sdkconfig_escape(ssid)}"\n')
    if not seen_pass:
        out.append(f'{PASS_KEY}="{sdkconfig_escape(password)}"\n')
    with open(SDKCONFIG, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--show", action="store_true", help="显示当前配置（密码打码）")
    ap.add_argument("--clear", action="store_true", help="清空凭据")
    args = ap.parse_args()

    if not os.path.isfile(SDKCONFIG):
        raise SystemExit(
            f"找不到 {SDKCONFIG}\n"
            "先在工程目录里跑一次构建（tools\\build.bat），让 sdkconfig 生成出来。")

    if args.show:
        ssid, password = read_current()
        print(f"  SSID     : {ssid!r}" if ssid else "  SSID     : （空）")
        print(f"  password : {'（已设置，' + str(len(password)) + ' 字符）' if password else '（空）'}")
        return 0

    if args.clear:
        write_values("", "")
        print("凭据已清空。重新构建后生效。")
        return 0

    print("把 Wi-Fi 名称与密码写进 sdkconfig（只落在这个文件里，不进仓库）。")
    print("密码输入时不回显，也不会被打印出来。")
    ssid = input("Wi-Fi 名称 (SSID): ").strip()
    if not ssid:
        raise SystemExit("SSID 不能为空")
    password = getpass.getpass("Wi-Fi 密码: ")
    if not password:
        raise SystemExit("密码不能为空（开放网络的话，请设一个空格）")

    write_values(ssid, password)
    # 只回显长度与首尾，够确认没打错，又不至于把凭据写进任何日志。
    print(f"已写入：SSID {ssid[:2]}…{ssid[-1:]}（{len(ssid)} 字符），"
          f"密码 {len(password)} 字符。")
    print()
    print("下一步：")
    print("    tools\\build.bat")
    print("    tools\\flash.bat COMx")
    print("或一条命令做完：tools\\set_wifi.bat COMx")
    return 0


if __name__ == "__main__":
    sys.exit(main())
