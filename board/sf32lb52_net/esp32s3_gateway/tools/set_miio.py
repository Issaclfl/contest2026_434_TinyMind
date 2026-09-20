#!/usr/bin/env python3
"""把米家设备表写进 sdkconfig —— token 是凭据，只该落在这个被 gitignore 的文件里。

为什么要单独做一个：设备表要填 IP 和一个 32 位十六进制的 token，而 token 等价于
那台设备的钥匙。它不该出现在命令行历史、聊天记录、日志或仓库里。所以这个脚本
默认用隐藏输入读 token，`--show` 也只打码显示，从不回显明文。

写法（`;` 分隔多台设备）：
    CONFIG_GATEWAY_MIIO_TABLE="lamp,192.168.1.23,<32 位 token>,yeelink.light.color3"

用法：
    python tools/set_miio.py --add lamp --ip 192.168.1.23 --model yeelink.light.color3
    python tools/set_miio.py --show        # 看现在配了什么（token 打码）
    python tools/set_miio.py --clear

写完要重新编译、烧写才生效（Kconfig 项变了）：
    tools\\build.bat
    tools\\flash.bat COMx
"""
import argparse
import getpass
import os
import re
import sys

KEY = "CONFIG_GATEWAY_MIIO_TABLE"
DEFAULT_SDKCONFIG = os.path.join(os.getcwd(), "sdkconfig")


def sdkconfig_escape(value):
    """sdkconfig 里字符串是带引号的 C 字符串：反斜杠与引号要转义。"""
    return value.replace("\\", "\\\\").replace('"', '\\"')


def mask(token):
    if not token:
        return "(空)"
    return "<%d 位，已隐藏>" % len(token)


def read_table(path):
    if not os.path.isfile(path):
        return None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(rf'{KEY}="(.*)"\s*$', line)
        if m:
            return m.group(1)
    return None


def parse_rows(table):
    rows = []
    for part in (table or "").split(";"):
        part = part.strip()
        if not part:
            continue
        fields = part.split(",")
        if len(fields) < 3:
            continue
        rows.append({
            "name": fields[0],
            "ip": fields[1],
            "token": fields[2],
            "model": fields[3] if len(fields) > 3 else "",
        })
    return rows


def render(rows):
    return ";".join(
        "%s,%s,%s,%s" % (r["name"], r["ip"], r["token"], r["model"]) for r in rows
    )


def write_table(path, table):
    if not os.path.isfile(path):
        print("找不到 %s —— 先在工程目录里跑一次 build.bat 让 sdkconfig 生成" % path)
        return False
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines(True)
    out, done = [], False
    for line in lines:
        if re.match(rf"{KEY}=", line):
            out.append('%s="%s"\n' % (KEY, sdkconfig_escape(table)))
            done = True
        else:
            out.append(line)
    if not done:
        out.append("\n# 米家设备表（token 是凭据，只在这个文件里）\n")
        out.append('%s="%s"\n' % (KEY, sdkconfig_escape(table)))
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.writelines(out)
    return True


def check_ip(ip):
    parts = ip.split(".")
    if len(parts) != 4:
        return False
    for p in parts:
        if not p.isdigit() or not 0 <= int(p) <= 255:
            return False
    return True


def main():
    ap = argparse.ArgumentParser(description="米家设备表 → sdkconfig（凭据不入库）")
    ap.add_argument("--add", metavar="NAME", help="设备逻辑名，例如 lamp（板子与模型用它）")
    ap.add_argument("--ip", help="设备局域网 IP")
    ap.add_argument("--token", help="32 位十六进制 token；不给就隐藏输入")
    ap.add_argument("--model", default="", help="例如 yeelink.light.color3（可空）")
    ap.add_argument("--show", action="store_true", help="显示当前表（token 打码）")
    ap.add_argument("--clear", action="store_true", help="清空设备表")
    ap.add_argument("--sdkconfig", default=DEFAULT_SDKCONFIG)
    args = ap.parse_args()

    table = read_table(args.sdkconfig)
    rows = parse_rows(table)

    if args.show or not (args.add or args.clear):
        print("sdkconfig : %s" % args.sdkconfig)
        if not rows:
            print("%s : (未配置)" % KEY)
        for r in rows:
            print("  %-8s %-16s %s %s" % (r["name"], r["ip"], mask(r["token"]), r["model"]))
        if not (args.add or args.clear):
            print("\n加设备： --add <名字> --ip <IP> [--model <型号>]")
        return 0

    if args.clear:
        if write_table(args.sdkconfig, ""):
            print("已清空 %s" % KEY)
        return 0

    name = args.add.strip()
    ip = (args.ip or "").strip()
    token = args.token
    if not ip or not check_ip(ip):
        print("IP 不合法：%r" % ip)
        return 2
    if not token:
        token = getpass.getpass("token（32 位十六进制，输入不回显）：").strip()
    token = token.lower()
    if not re.fullmatch(r"[0-9a-f]{32}", token):
        print("token 必须是 32 位十六进制字符（收到的长度 %d）" % len(token))
        return 2

    rows = [r for r in rows if r["name"] != name]
    rows.append({"name": name, "ip": ip, "token": token, "model": args.model.strip()})
    rows.sort(key=lambda r: r["name"])

    if not write_table(args.sdkconfig, render(rows)):
        return 1

    print("已写入 %s（%d 台设备；token 未回显）" % (KEY, len(rows)))
    for r in rows:
        print("  %-8s %-16s %s %s" % (r["name"], r["ip"], mask(r["token"]), r["model"]))
    print("\n下一步： tools\\build.bat  →  tools\\flash.bat COMx")
    return 0


if __name__ == "__main__":
    sys.exit(main())
