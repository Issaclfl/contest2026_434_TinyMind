# SF32LB52-DevKit-LCD：给一块没有网卡的板子接上网

## 为什么需要这一步

SF32LB52-DevKit-LCD 上**没有任何网卡**——vendor 树里不存在 WiFi 驱动，芯片也不带
以太网 MAC。板子自带的 USB 是片内 CDC ACM 串口（VID `0x38F4`，也就是控制台），
不是一个能做网络接口的 USB gadget。

于是开箱状态是：`CONFIG_NET` 关着，整板与外界只有一条串口调试线。

要把 IP 送上去，只有一条路：**在串口上跑点对点协议**。NuttX 树里现成的有两条：

| 方案 | 板侧代码 | PC 侧 | 结论 |
|---|---|---|---|
| SLIP | `nuttx/drivers/net/slip.c` | Linux 5.14 起已移除 SLIP 支持 | 用不了 |
| **PPP** | `apps/netutils/pppd` + `apps/examples/pppd` | `pppd` 是标配 | **采用** |

## 数据通路

```
板子 UART2 (PA20 RX / PA27 TX)
      |  3 根线：TX↔RX、RX↔TX、GND↔GND
  USB-TTL 转接器
      |  USB
Windows  COMx
      |  serial_tcp_bridge.py      监听 0.0.0.0:5555
      |  TCP
WSL  socat                      造出 /tmp/ttyPPP
      |  PTY
    pppd                        10.0.0.1 ←→ 10.0.0.2
      |  ppp0
    iptables MASQUERADE  →  eth0  →  外网
```

PC 侧之所以要绕这么一圈：`pppd` 要开 `/dev/ppp` 并改 iptables，只能在 WSL 里跑；
而 COM 口在 Windows 上。两条路都验证过是通的——

| 方向 | 结果 |
|---|---|
| WSL → Windows 宿主（`ip route` 默认网关，NAT 模式下即宿主） | 通 |
| Windows → WSL（`localhost:端口`，WSL2 端口转发） | 通 |

脚本用的是第一条。**宿主地址每次 WSL 重启都会变**（这里实测是 `172.18.176.1`），
所以 `ppp_up.sh` 里是自动探测，不要写死。

## 板侧改动（3 处，见 `patches/`）

### 0001 `apps/examples/pppd/pppd_main.c` —— 让例程支持直连串口

上游这个例程**只能连蜂窝模组**：tty 写死 `/dev/ttyS1`，连接脚本是一段
`AT+CGDCONT` / `ATD*99***1#` 的拨号对话。直连一台 PC 时没有模组可拨，
那段 chat 脚本会一直等不到 `CONNECT` 而超时。

改动后：

```
pppd                上行行为不变，在 /dev/ttyS1 上拨蜂窝模组
pppd <tty>          直连串口，默认 460800，不下发 chat 脚本
pppd <tty> <baud>   直连串口并指定波特率
```

**为什么一定要能指定波特率**：`pppd()` 只对 tty 做 `open()`，从不调 `tcsetattr`，
链路跑的是驱动初始化时的速率。本板 `CONFIG_UART_BAUD=1000000`，而很多 USB-TTL
（典型如 CP2102）最高只能到 921600，1 M 根本产生不出来。所以例程在交给 `pppd()`
之前先把速率设好。

### 0002 `apps/netutils/pppd/ppp_conf.h` —— 收缓冲 1024 → 2048

`PPP_RX_BUFFER_SIZE` 同时当两个用途：装一个完整 IP 包，以及在拆帧时装一个完整
AHDLC 帧。**帧比包大**：1500 的 MTU 加上协议与 FCS 字段，再算上 `0x7e`/`0x7d`
的转义膨胀（TLS 密文近似随机，约 0.8% 的字节要转义），实测尾部能到 1560 字节。
1024 装不下——这是把 `NET_TUN_PKTSIZE` 调到 1500 之后才暴露的问题。

### 0003 `vendor/sifli/chips/sf32lb52/Kconfig` —— 声明 `ARCH_HAVE_SERIAL_TERMIOS`

驱动里 `TCGETS` / `TCSETS` 的**代码已经写好且完整**
（`vendor/sifli/chips/sf32lb52/sifli_uart.c:278,304`），但它们被
`#ifdef CONFIG_SERIAL_TERMIOS` 包着，而 `CONFIG_SERIAL_TERMIOS` 依赖
`ARCH_HAVE_SERIAL_TERMIOS`，芯片 Kconfig 从未 `select` 它。

也就是说：**这个能力一直存在，只是没被声明出来**，于是运行时改不了任何串口的
波特率。加一行 `select` 即可，没有任何新增代码。

## 板级 defconfig

见 `../sf32lb52_audio/ai_agent_port/configs/ai_agent/defconfig`，网络相关部分：

| 符号 | 值 | 为什么 |
|---|---|---|
| `CONFIG_SERIAL_TERMIOS` | `y` | 让上面的 `TCSETS` 生效 |
| `CONFIG_EXAMPLES_PPPD` | `y` | 提供 nsh 的 `pppd` 命令 |
| `CONFIG_NETUTILS_PPPD` | `y` | PPP 状态机（由上一项 select） |
| `CONFIG_NET_TUN` | `y` | pppd 用它承载 IP（由上一项 select） |
| `CONFIG_NET_TUN_PKTSIZE` | `1500` | **这个值就是链路 MTU**，默认只有 296 |
| `CONFIG_NETDB_DNSCLIENT_NAMESERVER1/2` | `223.5.5.5` / `114.114.114.114` | 见下 |

**MTU 那一项是这里最容易踩的坑。** TUN 的包缓冲尺寸直接就是 ppp0 的 MTU，默认
296 意味着 MSS 只有 256——一次 TLS 握手的证书链会被切成二十个包。调到 1500 是
PPP 常规值，也让 `0002` 那个收缓冲改动成为必需。

**DNS 必须静态给**：板子的 `pppd` 不解析对端 IPCP 里的 DNS 选项
（`ppp_conf.h` 没有定义 `IPCP_GET_PRI_DNS` / `IPCP_GET_SEC_DNS`，整个 pppd 目录里
也没有任何一处调用 `netlib_set_dns`），所以解析器不会自己拿到服务器地址。

**不需要路由表。** `netdev_findby_ripv4addr()`
（`nuttx/net/netdev/netdev_findbyaddr.c`）在路由查找失败时会回退到
`netdev_default()`，而后者返回设备链表里第一个 UP 且非 loopback 的设备——ppp0
一 UP 就自动是默认出口。所以 `CONFIG_NET_ROUTE` 不必开。

## PC 侧

`pc_side/` 三个文件，都不需要管理员权限、不需要装驱动：

### 1. Windows：串口 ↔ TCP 桥

```powershell
python serial_tcp_bridge.py --list          # 先看有哪些 COM 口
python serial_tcp_bridge.py COM7 460800     # 默认监听 0.0.0.0:5555
```

只依赖 `pyserial`。有意不去动 DTR/RTS——有些板子把它们接到了复位或 boot 引脚，
而 pyserial 默认会在 `open()` 时拉高。

### 2. WSL：起链路

`socat` 与 `pppd` 需要先装（两者都不在默认镜像里）：

```bash
wsl.exe -d Ubuntu-24.04 -u root -e bash -lc \
  'apt-get update && apt-get install -y ppp socat'
```

用 `wsl -u root` 而不是 `sudo`：后者要密码，前者免密，而且 pppd 与 iptables
本来就必须是 root。

```bash
wsl.exe -d Ubuntu-24.04 -u root -e bash -lc \
  'bash <repo>/board/sf32lb52_net/pc_side/ppp_up.sh'
```

脚本做四件事：算出宿主地址与出口 MTU；起 `socat` 造 PTY；装好转发 / NAT /
MSS 夹取规则；起 `pppd` 并等 ppp0 出现。拆链路用 `ppp_down.sh`。

**为什么要夹 MSS**：板子的链路 MTU 是 1500，而 WSL 虚拟网卡通常只有 1280。
不夹的话板子会按 1500 发，WSL 往 eth0 转发时直接超 MTU。`ppp_up.sh` 在
`FORWARD` 链两个方向都把 SYN 的 MSS 设成 `出口MTU - 40`。

## 板子上怎么用

```
nsh> pppd /dev/ttyS0 460800
pppd: /dev/ttyS0 at 460800 baud, direct link
...
nsh> ifconfig
ppp0    Link encap:UNSPEC  HWaddr ...
        inet addr:10.0.0.2  Mask:255.255.255.255
nsh> ping 223.5.5.5
```

`/dev/ttyS0` 就是 UART2：控制台占着 UART1，串口驱动注册其余口时会跳过它，
所以 UART2 落到第一个空闲编号上。

## 实测记录

| 项目 | 结果 |
|---|---|
| `CONFIG_SERIAL_TERMIOS` 生效（`tcsetattr` 改 UART2 速率） | 待上板 |
| ppp0 建立、板子拿到 10.0.0.2 | 待上板 |
| 板子 ping 通外网 | 待上板 |
| `ai_agent` 经该链路访问 LLM 端点 | 待上板 |
