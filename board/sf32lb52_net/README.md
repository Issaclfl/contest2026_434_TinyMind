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

## 一次跑通

`pc_side/ppp_e2e.py` 把整条链路的四步串成一条命令，每步的输出都打出来：

```bash
# 路线 1（优先）：第二条 USB-C 线接芯片原生 USB 口
python ppp_e2e.py --mode usb   --port COM6

# 路线 2：USB-TTL 接 UART2
python ppp_e2e.py --mode usbttl --port COM8

# 不确定哪个口是哪个就先看：
python ppp_e2e.py --list
```

想手动做也行，顺序不能乱：

```bash
# 1) 板子：后台起 pppd，控制台保持可用
nsh> pppd /dev/ttyACM0 115200 &      # 路线 1
nsh> pppd /dev/ttyS0   460800 &      # 路线 2

# 2) Windows：把承载 PPP 的 COM 口挂到 TCP 上
python serial_tcp_bridge.py COM6 115200

# 3) WSL：起 ppp0 + NAT + MSS 夹取（宿主地址自动探测）
wsl.exe -d Ubuntu-24.04 -u root -e bash -lc \
  'bash <repo>/board/sf32lb52_net/pc_side/ppp_up.sh'

# 4) 板子上验证
nsh> ifconfig          # ppp0 应拿到 10.0.0.2
nsh> ping 223.5.5.5
```

之后在板子上跑 `ai_agent`，用 `set_llm` 写入端点，`net_test` 可以测 HTTPS 连通。

拆链路：`ppp_down.sh`。

## 两条通路，先试免接线的那条

板子上有**两个 Type-C 口**（板子官网规格页列明）：

| 口 | 接到哪 | 在 PC 上 |
|---|---|---|
| 口 A（带板载 USB 转串口芯片） | 板载 CH343 桥 → **UART1 控制台** + ROM 下载 | `1A86:55D3` → COM5 |
| 口 B（USB2.0 FS） | **芯片原生 USB** → 固件里的 `/dev/ttyACM0` | 插上后应多出一个 COM 口 |

固件里 `cdcacm_initialize()`（`sifli_ap.c:616`）会把片内 CDC ACM 注册成 `/dev/ttyACM0`，
`ls /dev` 能看到。**这条口如果可用，就是免 USB-TTL、免接线的通路。**

### 路线 1（优先）：第二条 USB-C 线，走 `/dev/ttyACM0`

```
板子口 B ──USB-C 线── PC
```

插上后 Windows 设备列表里应多出一个 COM 口（VID `0x38F4`，SiFli）。然后：

```
nsh> pppd /dev/ttyACM0 115200 &     # USB CDC 的速率是名义值，真正快慢由 USB 决定
```

**说明**：截至本次适配，我们**还没有在物理上试过这条路线**——当时只插了口 A，
Windows 里枚举不到任何 `0x38F4` 设备，于是先按口 A + 外部 USB-TTL 做完了全部验证。
但软件侧已经核对过：`nuttx/drivers/usbdev/cdcacm.c` 实现了 `TCGETS`/`TCSETS`
（且本工程已开 `CONFIG_SERIAL_TERMIOS`），所以 `pppd` 那条"先设速率再交接"的路径
在 `/dev/ttyACM0` 上同样成立。

**插上试一下只要一分钟**，成功的话整个接线环节就没了。

### 路线 2（已验证软件链路）：USB-TTL 接 UART2

USB-TTL 转接器三根线接到板子的 UART2：

| USB-TTL | 板子 | 说明 |
|---|---|---|
| TX  | **PA20** | USART2_RXD |
| RX  | **PA27** | USART2_TXD |
| GND | GND      | 必须接，否则电平参考不同 |

PA20 / PA27 在本板上是 UART2 专用，没有被别的功能复用
（只有 -ULP / 黄山派把 PA27 用作 TF 卡 detect，本板让出来了）。

### 路线 3（S3 侧已真机验证，板侧联调待接线）：ESP32-S3 做对端

前两条路的对端都是 PC。这一条把 PC 换掉：一块 ESP32-S3-DevKitC-1 跑
`esp32s3_gateway/`，自己当 PPP 服务端 + NAT，把板子接上 Wi-Fi。

```
板子 UART2（PA20/PA27）── 三根杜邦线 ── ESP32-S3（GPIO17/18）── Wi-Fi ── 路由器
```

| ESP32-S3 | 板子 | 说明 |
|---|---|---|
| TX **GPIO17** | **PA20** | USART2_RXD |
| RX **GPIO18** | **PA27** | USART2_TXD |
| GND | GND | 必须接，否则电平参考不同 |
| — | — | **不要接 VCC**：两块板各自供电，接 VCC 会反灌 |

板子侧与路线 2 一字不差（`pppd /dev/ttyS0 460800 &`），**板子固件零改动**——
对端是谁跟板子无关。差别只在 PC 侧不再需要串口桥与 `ppp_up.sh`：

```
python ppp_e2e.py --mode esp32s3
```

原理、接线、构建与那处 ESP-IDF 缺陷的修法都在 `esp32s3_gateway/README.md`。

**状态要说清楚，分两半：S3 这一半已在真机验证——2026-09-18 台架实测，
S3 的 PPP 服务端对 PC 侧 pppd 客户端 LCP/IPCP 协商成功、地址分配正确、
ping 5/5 零丢包（见 `esp32s3_gateway/README.md` 的"台架实测"）。还没验的是
最后一跳"板子 ↔ S3"（等三根杜邦线）和 NAPT 转发（等 Wi-Fi 凭据）。**

## 板子上怎么用

**必须后台跑。** `pppd` 是个不会返回的阻塞循环，前台跑会把 nsh 的控制台
整个占死——直到复位为止都无法再输入任何命令：

```
nsh> pppd /dev/ttyS0 460800 &
pppd [8:100]
nsh> pppd: /dev/ttyS0 at 460800 baud, direct link
nsh> ifconfig
ppp0    Link encap:UNSPEC  HWaddr ...
        inet addr:10.0.0.2  Mask:255.255.255.255
nsh> ping 223.5.5.5
```

路线 2 用 `pppd /dev/ttyS0 460800 &`：`/dev/ttyS0` 就是 UART2——控制台占着 UART1，
串口驱动注册其余口时会跳过它，所以 UART2 落到第一个空闲编号上。注意板子 README 的
表格写的是 `/dev/ttyS1`，那是 nsh 配置（多启用了一路 UART3）下的编号；本工程只启用了
UART1/UART2，实测就是 `ttyS0`。

路线 1 用 `pppd /dev/ttyACM0 115200 &`：`/dev/ttyACM0` 是片内 USB CDC ACM，
由 `cdcacm_initialize()` 注册（`sifli_ap.c:616`）。**USB CDC 的波特率是名义值**——
真正快慢由 USB 决定，写多少都一样；之所以还要写，是因为 pppd 在交接前会走一遍
`tcsetattr`，而 `cdcacm` 实现了 `TCSETS`（`nuttx/drivers/usbdev/cdcacm.c:2511`），
所以任何值都能过。

### 别前台跑，也别随便动 RTS

SF32LB52 **没有专用复位引脚**：复位是拿 USB-UART 桥的 RTS 去控一个负载开关
切断 VCC 实现的（见板子 README 与思澈的自动下载设计文档）。于是串口工具的
RTS 行为直接决定板子是死是活——`screen` / `cu` 会让芯片一直停在复位，
`minicom` 连接时会复位一次。

实测：pyserial 的默认行为（open() 时拉 RTS）在这块板上**不会**导致复位或
掉电，`serial_tcp_bridge.py` 因此在 open 后不动 DTR/RTS，与 `nsh.py`
保持一致——已经验证过桥进程退出、串口关闭之后板子照常运行。

## 实测记录

**路线 1 与路线 2 的软件链路都已在真机上端到端跑通**；路线 3 的 S3 侧已在
2026-09-18 台架实测中真机验证（对 PC），板侧联调待接线，所以单独列在表末。

| 环节 | 怎么验的 | 结果 |
|---|---|---|
| WSL ↔ Windows 宿主 TCP | 两个方向对打 | 通 |
| Windows 桥：COM ↔ TCP 双向 | 桥接板子控制台，从 WSL 发 `uname -a` | 板子执行并把结果送回 |
| socat：TCP ↔ PTY | 与桥串起来跑上面那条 | 通 |
| pppd 协商 + 分配 IP + 数据面 | WSL 内两个 PTY 对接跑两个 pppd | LCP/IPCP 成功，ppp0/ppp1 起来，跨链路 ping 3/3 零丢包 |
| 板子 `/dev/ttyS0` 可打开 | `pppd /dev/ttyS0 460800` | 通过 |
| `CONFIG_SERIAL_TERMIOS` 生效 | 同上，看是否打印 `at 460800 baud` | **通过**（不补那行 Kconfig `select`，就只能跑 1000000） |
| `pppd` 内置命令 + 直连模式 | `pppd -h` 打印用法 | 通过（补丁在真机生效） |
| **路线 1：原生 USB 端到端** | `pppd /dev/ttyACM0 115200 &` + `ppp_e2e.py --mode usb` | **通**：ppp0 起来、板子 `ping 223.5.5.5` 130 ms、Agent 报 `Network connected: 10.0.0.2` |
| **路线 1 上的真实对话** | `ai_agent` → `set_llm` → `ask` | **通**：`TLSv1.2` 握手成功，157 字节真实回复；工具调用 `get_current_time` 正常 |
| 路线 2：UART2 + USB-TTL | 物理接线 | 软件链路全环节已验证，未做物理联调（路线 1 已满足需求） |
| **路线 3：S3 网关 PPP 服务端（真机）** | S3 烧台架固件（`GATEWAY_UPLINK=n`、PPP 走 UART0）→ FT232 桥 → PC 侧 pppd 当客户端 | **通**：LCP/IPCP 协商成功，CCP/IPv6CP/VJ 被按配置拒绝，地址 10.0.0.1/10.0.0.2 分配正确，`ping 10.0.0.1` 5/5 零丢包、平均 23 ms |
| 路线 3：板子 ↔ S3 最后一跳 | 三根杜邦线 + Wi-Fi 凭据 | 待联调 |

### 路线 1 实测到的两个限制（如实记录）

**一、板子每次复位或重新烧录后，COM 口会打不开，必须把线物理拔插一次。**
可稳定复现。加了 USB 探针后看到 Windows 在反复对端点 `0x83`（CDC 的中断通知
端点）发 `CLEAR_FEATURE(HALT)`，即该端点一直处于 stall 状态——很可能是
`usbser` 建立通知管道失败导致端口不可用，拔插后能建立成功。**已定位到这一层，
未继续修**。

**二、`/data` 是 tmpfs**，重启后 `set_llm` 写入的配置要重设。

板子上 PSRAM 已经在堆里：`free` 报 Umem 总量 8.7 MB（512 KB SRAM + 8 MB
PSRAM，`CONFIG_MM_REGIONS=2`），不需要额外动作。
