#!/usr/bin/env python3
"""一条命令跑通板子的 PPP 链路（在 Windows 上运行）。

把链路搭起来要做的四件事串成一步：板子起 pppd → Windows 起串口桥 →
WSL 起 ppp0 + NAT → 从板子控制台验证。

    [板子] ──串口── [COMx]
                      |  serial_tcp_bridge.py
                      |  TCP
       WSL socat -> /tmp/ttyPPP -> pppd -> ppp0
                                          |
                            MASQUERADE -> eth0 -> 外网

三条通路
--------
前两条由 PC 做对端，第三条由另一块板子做对端。

  路线 1（优先，免接线）
      用第二条 USB-C 线接板子的「USB2.0 FS」口。那是芯片原生 USB，
      固件把它注册成 /dev/ttyACM0。插上后 Windows 里会多出一个 COM 口。
        python ppp_e2e.py --mode usb --port COM6

  路线 2（软件链路已完整验证）
      用 USB-TTL 转接器接板子 UART2：TX→PA20、RX→PA27、GND→GND。
      板子侧是 /dev/ttyS0。
        python ppp_e2e.py --mode usbttl --port COM8

  路线 3（ESP32-S3 做对端，不需要 PC）
      ESP32-S3-DevKitC-1 跑 esp32s3_gateway（见 ../esp32s3_gateway/），
      它自己当 PPP 服务端 + NAT，把板子接上 Wi-Fi。此时 PC 上的串口桥与
      ppp_up.sh 都不需要，本脚本只负责把板子的 pppd 拉起来并验证。
        python ppp_e2e.py --mode esp32s3

前提
----
1. 板子上跑着含 pppd 的固件（ai_agent 那份就是）
2. WSL 里装好 ppp 与 socat：
     wsl.exe -d Ubuntu-24.04 -u root -e bash -lc 'apt-get install -y ppp socat'
3. 控制台串口（默认 COM5）没被别的程序占着

说明
----
每一步的原始输出都打出来，不做黑箱判断——链路上任何一段断了都能看出是哪一段。
控制台操作复用 nsh.py 的交互方式。
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

# 两条通路的差异集中在这里
MODES = {
    # 芯片原生 USB CDC ACM。速率是名义值——真正快慢由 USB 决定，
    # 但 pppd 仍会走一遍 tcsetattr，而 cdcacm 实现了 TCSETS，所以能过。
    "usb": {
        "board_tty": "/dev/ttyACM0",
        "baud": 115200,
        "hint": "板子的 USB2.0 FS 口（芯片原生 USB）",
    },
    # UART2 + USB-TTL。UART1 是控制台，串口驱动注册其余口时会跳过它，
    # 所以本工程只启用 UART1/UART2 时 UART2 落在 ttyS0 上。
    "usbttl": {
        "board_tty": "/dev/ttyS0",
        "baud": 460800,
        "hint": "USB-TTL → 板子 UART2（TX→PA20、RX→PA27、GND→GND）",
        "check_dev": True,
    },
    # ESP32-S3 做对端：它自己就是 PPP 服务端 + NAT，PC 侧没有桥、也没有
    # ppp0 要建，所以 peer 一栏让主流程跳过步骤 2、3。
    "esp32s3": {
        "board_tty": "/dev/ttyS0",
        "baud": 460800,
        "hint": "ESP32-S3 网关 → 板子 UART2（TX→PA20、RX→PA27、GND→GND）",
        "peer": "esp32s3",
        "check_dev": True,
    },
}


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
    print("提示：板载 CH343 桥（口 A，控制台）是 1A86:55D3，通常枚举为 COM5。")
    print("      承载 PPP 的是另一个口：")
    print("        --mode usb     找 VID:PID = 38F4:xxxx（SiFli，芯片原生 USB）")
    print("        --mode usbttl  找 USB-TTL 那个（不是 1A86:55D3、也不是蓝牙）")


def to_wsl_path(path):
    """把本脚本所在目录换成 WSL 看得懂的路径。

    脚本常被复制到 Windows 工作目录下运行，而 ppp_up.sh 要交给 WSL 执行；
    WSL 不认 "C:\\..."，得换成 /mnt/c/...。
    """

    full = os.path.abspath(path)
    drive, rest = os.path.splitdrive(full)
    if drive:
        return "/mnt/" + drive[0].lower() + rest.replace("\\", "/")
    return full.replace("\\", "/")


def wsl(command, timeout=180):
    """在 WSL 里以 root 执行命令并返回输出（pppd 与 iptables 都需要 root）"""
    full = ["wsl.exe", "-d", WSL_DISTRO, "-u", "root", "-e", "bash", "-lc", command]
    proc = subprocess.run(full, capture_output=True, text=True, timeout=timeout)
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


class Nsh:
    """板子控制台：写一行命令，读到线路静默为止"""

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
    parser.add_argument("--mode", choices=sorted(MODES), default="usb",
                        help="usb = 芯片原生 USB（免接线，先试这个）；"
                             "usbttl = UART2 + USB-TTL；"
                             "esp32s3 = 由 ESP32-S3 网关做对端，不经过 PC（默认 usb）")
    parser.add_argument("--port", help="承载 PPP 的那个串口，例如 COM6")
    parser.add_argument("--console", default="COM5", help="板子控制台串口（默认 COM5）")
    parser.add_argument("--console-baud", type=int, default=1000000,
                        help="控制台波特率（本板固定 1000000）")
    parser.add_argument("--board-tty", help="覆盖板子侧的设备节点")
    parser.add_argument("--baud", type=int, help="覆盖链路波特率")
    parser.add_argument("--list", action="store_true", help="列出串口后退出")
    parser.add_argument("--stop", action="store_true",
                        help="拆掉链路：停掉串口桥并清掉 WSL 侧的 ppp0 与规则")
    args = parser.parse_args()

    if args.stop:
        if args.mode == "esp32s3":
            print("esp32s3 模式在 PC 侧没有串口桥、也没有 ppp0 需要拆。")
            print("要重来一次：复位 ESP32-S3，或在板子上重跑 `pppd /dev/ttyS0 460800 &`。")
            return 0
        rc, out = wsl(f'bash "{to_wsl_path(HERE)}/ppp_down.sh"')
        print(out.strip())
        subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | "
             "Where-Object { $_.CommandLine -like '*serial_tcp_bridge*' } | "
             "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"],
            capture_output=True)
        print("串口桥已停")
        return rc

    if args.list or not args.port:
        list_ports_briefly()
        return 0 if args.list else 1

    preset = MODES[args.mode]
    board_tty = args.board_tty or preset["board_tty"]
    baud = args.baud or preset["baud"]

    print(f"通路：{preset['hint']}")
    print(f"      板子侧 {board_tty}，链路 {baud} baud，桥接在 {args.port}")

    step(1, "在板子上后台起 pppd")
    nsh = Nsh(args.console, args.console_baud)
    print(nsh.run("uname -a").strip()[:200])
    if preset.get("check_dev"):
        print("→ 先确认板子认到了这个节点：")
        print(nsh.run("ls /dev").strip()[-500:])

    # 已经有一个在跑就别再起第二个：NuttX 的串口允许多次 open，
    # 两个 pppd 会往同一条线上各发各的帧，谁也谈不成。
    running = nsh.run("ps")
    if "pppd" in running:
        print("→ 板子上已经有 pppd 在跑，跳过启动：")
        for line in running.splitlines():
            if "pppd" in line:
                print("     " + line.strip())
    else:
        print("→ pppd 必须后台跑：它不会返回，前台会把控制台整个占死")
        print(nsh.run(f"pppd {board_tty} {baud} &").strip()[:400])
    nsh.close()

    if preset.get("peer") == "esp32s3":
        step(2, "PC 侧无需操作：对端就是 ESP32-S3 网关")
        print("S3 自己当 PPP 服务端 + NAT，地址分配、默认路由、NAPT 都在它那侧完成，")
        print("所以这边没有串口桥，也没有 WSL 的 ppp0 要起。")
        print()
        print("上电确认三件事：")
        print("  · S3 已烧 esp32s3_gateway 固件且连上了 Wi-Fi（看它的串口日志里 \"uplink ready\"）")
        print(f"  · 波特率对齐：板子 {baud}  =  S3 的 GATEWAY_UART_BAUD")
        print("  · 接线：S3 TX → PA20、S3 RX → PA27、GND 对 GND（不要接 VCC）")
        print()
        print("→ S3 那边会打出 \"session up: we are 10.0.0.1, board is 10.0.0.2\"，")
        print("  以及 \"NAPT on\"；如果只有前者没有后者，板子能到 S3 但出不了网。")
    else:
        step(2, "在 Windows 上把串口桥接到 TCP")
        bridge = os.path.join(HERE, "serial_tcp_bridge.py")
        bridge_log = os.path.join(HERE, "bridge.log")
        print(f"→ 起 {os.path.basename(bridge)} {args.port} {baud}")

        # 桥必须活得比本脚本久。若把它的 stdout 接成管道，本脚本一退出管道就断，
        # 桥写日志时报错退出，WSL 侧的 socat 随即失去对端、ppp0 消失——链路会
        # 看起来"刚建好就断"。所以日志写文件，并让它脱离本进程。
        log_fh = open(bridge_log, "w")
        bridge_proc = subprocess.Popen(
            [sys.executable, "-u", bridge, args.port, str(baud), "--tcp-port", "5555"],
            stdout=log_fh, stderr=subprocess.STDOUT,
            creationflags=getattr(subprocess, "DETACHED_PROCESS", 0))
        time.sleep(3)
        if bridge_proc.poll() is not None:
            print("桥启动失败：")
            with open(bridge_log, encoding="utf-8", errors="replace") as fh:
                print(fh.read())
            return 1
        print(f"桥已常驻（pid {bridge_proc.pid}，日志 {bridge_log}）")

        step(3, "在 WSL 里起 ppp0 + NAT + MSS 夹取")
        # 宿主地址留空，交给 ppp_up.sh 自己探测：桥在 Windows 上，而 WSL 里的
        # 127.0.0.1 是 WSL 自己，填它必然连不上。路径也要引起来——工作目录名里
        # 常有空格与中文。
        rc, out = wsl(f'bash "{to_wsl_path(HERE)}/ppp_up.sh" '
                      f'"" 5555 10.0.0.1 10.0.0.2 {baud}')
        print(out.strip()[-2000:])
        if rc != 0:
            print(f"→ ppp_up.sh 返回 {rc}，链路没起来。上面的日志能看出卡在哪一段。")
            bridge_proc.terminate()
            return 1

    step(4, "从板子控制台验证")
    nsh = Nsh(args.console, args.console_baud)
    print(nsh.run("ifconfig").strip()[-1200:])
    print()
    print(nsh.run("ping 223.5.5.5").strip()[-1200:])
    print()
    print("→ 之后在板子上跑 ai_agent，用 set_llm 写入 LLM 端点即可对话：")
    print("     nsh> ai_agent")
    print("     vela> set_llm https://<host>/v1 <model> <api_key>")
    print("     vela> ask 你好")
    nsh.close()

    print()
    print("━━━ 完成 " + "━" * 46)
    print("拆链路：python ppp_e2e.py --stop")
    print("（链路建好后可以放心退出本脚本：桥与 ppp0 都会继续存在）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
