#!/usr/bin/env python3
"""一条命令跑通板子的 PPP 链路（在 Windows 上运行）。

把接线之后要做的四件事串起来：板子起 pppd → Windows 起串口桥 →
WSL 起 ppp0 + NAT → 从板子控制台验证。

    [板子 UART2] --三根线--> [USB-TTL] --USB--> [COMx]
                                                   |  serial_tcp_bridge.py
                                                   |  TCP
                                    WSL socat -> /tmp/ttyPPP -> pppd -> ppp0
                                                                        |
                                                          MASQUERADE -> eth0 -> 外网

前提
----
1. 已经接好线：USB-TTL 的 TX→板子 PA20、RX→PA27、GND→GND
2. 板子上跑着含 pppd 的固件（ai_agent 那份就是）
3. WSL 里装好 ppp 与 socat：
     wsl.exe -d Ubuntu-24.04 -u root -e bash -lc 'apt-get install -y ppp socat'
4. 板子的控制台串口（默认 COM5）没有被别的程序占着

用法
----
    python ppp_e2e.py --usbttl COM8
    python ppp_e2e.py --usbttl COM8 --console COM5 --baud 460800
    python ppp_e2e.py --list

说明
----
本脚本把每一步的原始输出都打出来，不做"成功/失败"的黑箱判断——链路上
任何一段出问题，都能从输出里看出是哪一段。板的控制台操作复用 nsh.py。
"""

import argparse
import os
import subprocess
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.stderr.write("pyserial is required:  pip install pyserial\n")
    raise SystemExit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
WSL_DISTRO = "Ubuntu-24.04"

# 板子上的 UART2 在本工程的配置下注册为 ttyS0（UART1 是控制台，被跳过）
BOARD_TTY = "/dev/ttyS0"


def list_ports_briefly():
    ports = list(list_ports.comports())
    if not ports:
        print("没有找到串口")
        return
    print(f"{'端口':<8} {'VID:PID':<12} 说明")
    for p in ports:
        vidpid = f"{p.vid:04X}:{p.pid:04X}" if p.vid else "-"
        print(f"{p.device:<8} {vidpid:<12} {p.description}")
    print()
    print("提示：板载的 USB-UART 桥是沁恒 CH343，VID:PID = 1A86:55D3")
    print("      （它通到 UART1，也就是控制台，通常枚举为 COM5）。")
    print("      USB-TTL 是另一个端口——找那个不是 1A86:55D3、也不是蓝牙的。")


def wsl(command, timeout=180):
    """在 WSL 里以 root 执行命令并返回输出（pppd 与 iptables 都需要 root）"""
    full = ["wsl.exe", "-d", WSL_DISTRO, "-u", "root", "-e", "bash", "-lc", command]
    proc = subprocess.run(full, capture_output=True, text=True, timeout=timeout)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


class Nsh:
    """板子控制台（复用 nsh.py 的交互方式：CRLF 结尾，读到静默为止）"""

    IDLE = 0.6
    LIMIT = 30.0

    def __init__(self, port, baud):
        self.port = serial.Serial(port, baud, timeout=0.1)
        time.sleep(0.4)
        self.port.reset_input_buffer()

    def run(self, command):
        self.port.reset_input_buffer()
        self.port.write(command.encode() + b"\r\n")
        buf, last, started = "", time.time(), time.time()
        while True:
            data = self.port.read(4096)
            if data:
                buf += data.decode("utf-8", "replace")
                last = time.time()
            now = time.time()
            if buf and now - last >= self.IDLE:
                break
            if now - started >= self.LIMIT:
                break
        return buf

    def close(self):
        self.port.close()


def step(n, title):
    print()
    print(f"━━━ 步骤 {n}：{title} " + "━" * max(0, 44 - len(title)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--usbttl", help="USB-TTL 的串口，例如 COM8")
    parser.add_argument("--console", default="COM5", help="板子控制台串口（默认 COM5）")
    parser.add_argument("--baud", type=int, default=460800,
                        help="PPP 链路波特率（默认 460800，两侧必须一致）")
    parser.add_argument("--console-baud", type=int, default=1000000,
                        help="控制台波特率（本板固定 1000000）")
    parser.add_argument("--list", action="store_true", help="列出串口后退出")
    args = parser.parse_args()

    if args.list or not args.usbttl:
        list_ports_briefly()
        return 0 if args.list else 1

    step(1, "在板子上后台起 pppd")
    nsh = Nsh(args.console, args.console_baud)
    print(nsh.run("uname -a").strip()[:200])
    print("→ pppd 必须后台跑：它不会返回，前台会把控制台整个占死")
    print(nsh.run(f"pppd {BOARD_TTY} {args.baud} &").strip()[:400])
    nsh.close()

    step(2, "在 Windows 上把 USB-TTL 桥接到 TCP")
    bridge = os.path.join(HERE, "serial_tcp_bridge.py")
    print(f"→ 起 {os.path.basename(bridge)} {args.usbttl} {args.baud}")
    bridge_proc = subprocess.Popen(
        [sys.executable, "-u", bridge, args.usbttl, str(args.baud),
         "--tcp-port", "5555"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(3)
    if bridge_proc.poll() is not None:
        print("桥启动失败：")
        print(bridge_proc.stdout.read())
        return 1
    print("桥已在后台运行（本脚本退出前一直有效）")

    step(3, "在 WSL 里起 ppp0 + NAT + MSS 夹取")
    rc, out = wsl(f"bash {HERE.replace(chr(92), '/')}/ppp_up.sh "
                  f"127.0.0.1 5555 10.0.0.1 10.0.0.2 {args.baud}")
    print(out.strip()[-2000:])
    if rc != 0:
        print(f"→ ppp_up.sh 返回 {rc}，链路没起来。上面的日志能看出卡在哪一段。")
        bridge_proc.terminate()
        return 1

    step(4, "从板子控制台验证")
    nsh = Nsh(args.console, args.console_baud)
    print(nsh.run("ifconfig").strip()[-1200:])
    print()
    print(nsh.run("ping 223.5.5.5", ).strip()[-1200:])
    print()
    print("→ 之后在板子上跑 ai_agent，用 set_llm 写入 LLM 端点即可对话：")
    print("     nsh> ai_agent")
    print("     vela> set_llm https://<host>/v1 <model> <api_key>")
    print("     vela> ask 你好")
    nsh.close()

    print()
    print("━━━ 完成 " + "━" * 46)
    print("拆链路：wsl.exe -d %s -u root -e bash -lc 'bash %s/ppp_down.sh'"
          % (WSL_DISTRO, HERE.replace("\\", "/")))
    print("（本脚本退出不会关掉桥；要停它请结束对应的 python 进程）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
