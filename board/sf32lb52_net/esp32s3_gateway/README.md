# ESP32-S3 Wi-Fi 网关（第 3 层的第三种承载）

把 `../pc_side/` 里由 PC 扮演的角色搬到一块 **ESP32-S3-DevKitC-1** 上：S3 跑
PPP 服务端 + NAPT，把板子接上 Wi-Fi。板子侧照旧当 PPP 客户端，
**板侧固件一行都不用改**——同一句 `pppd /dev/ttyS0 460800 &`，以前拨接在
USB-TTL 上的 PC，现在拨这块 S3。

```
板子 UART2（PA20/PA27）── 三根杜邦线 ── ESP32-S3（GPIO17/18）── Wi-Fi ── 路由器
```

## 状态（先看这段）

| 项 | 状态 |
|---|---|
| 软件 | 完成 |
| `idf.py build` | **通过**（esp32s3 目标，产出 `sf32lb52_wifi_gateway.bin` 355 KB） |
| 关键配置 | 已核对生成的 `sdkconfig.h`：`LWIP_PPP_SERVER_SUPPORT=1`、`LWIP_PPP_SUPPORT=1`、`LWIP_IPV4_NAPT=1`、`LWIP_IP_FORWARD=1`、`LWIP_PPP_NOTIFY_PHASE_SUPPORT=1`；VJ 头压缩 / PAP / CHAP / LCP echo 均未启用（正是要的） |
| 胶水层确实进了构建 | 已核对 `esp_netif_lwip_ppp.c.obj` 与 `lwip/netif/ppp.c.obj` 存在于构建产物中——没有下面那行修改，它们不会被编译，链接必然失败 |
| 编译期守卫 | 5 处误配做成了 `#error`，见 `main/net_ppp.c` |
| **上板、与板子的互通、NAPT 转发、断线重连** | **未验证** |

下面写的接线与验证步骤是**打算怎么做**，不是**已经验过**。这一条与
`../README.md` 里路线 1/2 的性质不同，不要混为一谈。

## 硬件

- ESP32-S3-DevKitC-1
- 三根杜邦线
- ESP-IDF **v5.5.5**（本机在 `E:\Espressif`；其他版本请先看"为什么要改 IDF"一节）

## 接线

| ESP32-S3 | 板子 | 说明 |
|---|---|---|
| TX **GPIO17** | **PA20** | USART2_RXD |
| RX **GPIO18** | **PA27** | USART2_TXD |
| GND | GND | 必须接，否则电平参考不同 |
| — | — | **不要接 VCC**：两块板各自供电，接 VCC 会反灌 |

板子侧 UART2 是 `/dev/ttyS0`：控制台占着 UART1，串口驱动注册其余口时会跳过它。
GPIO17/18 是 S3 上 UART1 的 IOMUX 原生引脚，不用额外路由；若排针上引不出来，
换成任意空闲脚即可（UART 可经 GPIO 矩阵路由）——在 `menuconfig` 里改
`GATEWAY_UART_TX_GPIO` / `GATEWAY_UART_RX_GPIO`，避开 19/20（原生 USB）、
22–25（S3 上不存在）、26–37（SPI flash / PSRAM，视模组而定）、43/44（控制台）。

## 构建与烧录

Wi-Fi 凭据**不进仓库**：`sdkconfig` 被 `.gitignore` 排除，凭据只留在本机。

```
tools\menuconfig.bat     REM SF32LB52 gateway → Wi-Fi SSID / Wi-Fi password
tools\build.bat          REM 设好 IDF 环境 + 修下面那处 IDF 缺陷 + 编译
idf.py -p COMx flash monitor
```

两个 .bat 存在的理由（都写在 `tools/idf_env.bat` 里）：这台机器上 ESP-IDF 不在
PATH 里；`idf_cmd_init.bat` 只装 DOSKEY 宏，非交互式 shell 里等于没装；而且 PATH
上的系统 Python 3.13 会让 export 脚本去找一个不存在的 `idf5.5_py3.13_env`，所以
IDF 自带的 3.11 要放到最前面。

## 跑起来

1. 烧好 S3，等它打出 `uplink ready: ...`（拿到 Wi-Fi 地址）
2. 板子：`pppd /dev/ttyS0 460800 &`
3. S3 应打出 `session 1 up: we are 10.0.0.1, board is 10.0.0.2` 和 `NAPT on`
4. 板子：`ifconfig`（`ppp0` = 10.0.0.2）、`ping 223.5.5.5`，然后 `ai_agent`

也可以一条命令拉起板子那侧：

```
python ..\pc_side\ppp_e2e.py --mode esp32s3
```

**S3 重启后板子要重跑一次 `pppd`**：实测板子的 `ppp0` 在对端消失后不会自己掉，
`net_status` 仍报 connected 而包已经不走了。S3 侧的日志里也写了这条提示。

## 台架模式：先只验 PPP，不开 WiFi

`GATEWAY_UPLINK=n` 时跳过 WiFi 与 NAPT，只起 PPP 服务端。用处是把两个未知数分开：
**「两边的 PPP 实现能不能谈成」** 与 **「NAT 转不转」**。前者才是整条路线唯一真正
没底的地方——对面是 NuttX 移植的 pppd，不是 Linux 的——而且它不开 WiFi 就能测，
不必先准备凭据。

```
tools\menuconfig.bat     REM SF32LB52 gateway → 关掉 "Bring up Wi-Fi and NAT..."
tools\build.bat
```

这个模式下板子只能到本设备、出不了公网；要出网得由 PC 侧或别的设备提供路由
（例如照 `../pc_side/ppp_up.sh` 那样做）。日志里会明确写出这一点，
免得把"本来就没有上行"误判成"NAPT 坏了"。

## 为什么要改 IDF 的一行（`tools/fix_idf_ppp_gate.py`）

`components/esp_netif/CMakeLists.txt` 这样决定要不要编译 PPP 胶水层：

```cmake
if(CONFIG_PPP_SUPPORT)
    list(APPEND srcs_lwip lwip/esp_netif_lwip_ppp.c lwip/netif/ppp.c)
endif()
```

而 `esp_netif_lwip_ppp.c` 正是 `esp_netif_ppp_set_params()`、
`esp_netif_start_ppp()` 的所在地。问题在于 **`CONFIG_PPP_SUPPORT` 只以
"兼容性别名"的形式存在**——ESP-IDF 生成的 `sdkconfig.h` 里有：

```c
#define CONFIG_PPP_SUPPORT CONFIG_LWIP_PPP_SUPPORT
```

（和 `CONFIG_SUPPORT_TERMIOS` 等一大批旧名字并列）。C 代码用它没问题，
但 CMake 的 `if()` 读的是从 `sdkconfig` 生成的变量，那里只有
`CONFIG_LWIP_PPP_SUPPORT`。于是这个 `if()` 恒为假，PPP 胶水层根本不参与编译，
链接期才会报 `esp_netif_ppp_set_params` 之类的未定义引用——这也解释了为什么
5.5.5 不再附带任何 PPP 示例。

**试过但不行的修法**：自己在 Kconfig 里声明一个 `PPP_SUPPORT`。这样 CMake 那侧
能过，但 confgen 会额外写上 `#define CONFIG_PPP_SUPPORT 1`，和上面那个别名
撞成"重定义"，在 `-Werror` 下直接编译失败。

**采用的修法**：把那个 `if()` 指向真正存在的符号。一行，且本就是它想表达的意思。

```
if(CONFIG_PPP_SUPPORT)  →  if(CONFIG_LWIP_PPP_SUPPORT)
```

`tools/fix_idf_ppp_gate.py` 做这件事，幂等（每次编译前跑都安全），并在输出里
打印它改了什么。**它改的是本机 IDF 安装里的一个文件**，不是本项目的一部分；
换一台机器或重装 IDF 后需要重跑（`tools\build.bat` 会自动跑）。

这与板子缺 `ARCH_HAVE_SERIAL_TERMIOS`（`../patches/0003`）是同一类缺陷：
**代码是完整的，开启它的那个开关写错了。**

## 设计要点（几处容易写错、且运行时不会报错的地方）

1. **NAPT 开在 PPP 接口上，不是 Wi-Fi 接口**。esp_netif 的说明是"开在朝向目标
   网络的那一侧"，实现上还同时只允许一个接口开 NAPT。所以顺序是：Wi-Fi 拿到
   地址 → 建 PPP 接口 → `action_start` → 接口 up 之后才 `esp_netif_napt_enable()`。
2. **服务端参数必须在 `esp_netif_action_start()` 之前设置**。`esp_netif_start_ppp()`
   在那一步读取它们，并按 `ppp_passive` 决定调用 `ppp_listen()` 还是
   `ppp_connect()`——设晚了只能重来一次会话。
3. **VJ 头压缩必须关**（`CONFIG_LWIP_PPP_VJ_HEADER_COMPRESSION=n`）。它的 Kconfig
   帮助文本自己写着：用 NAPT 时压缩过的包头可能解不开、包被丢掉——而这是静默的。
4. **`uart_set_rx_timeout(1)` 必须设**。不设的话串口驱动只在 FIFO 到达阈值时才
   抛事件，而 LCP/IPCP 来回的都是短帧，永远到不了阈值，协商会以"两边都没动静"
   的样子卡死。
5. **五处误配做成了编译期 `#error`**：PPP 未编译、服务端支持未开、VJ 未关、
   NAPT 未开、阶段事件未开。它们运行时全是静默失败，在只有一路控制台、没有调试器
   的板子上排查要花一下午，编译失败只要一分钟。
6. **MTU 对齐不需要额外动作**：板侧链路 MTU 由 `CONFIG_NET_TUN_PKTSIZE` 固定为
   1500，lwIP 的 `PPP_MRU` 默认正好也是 1500（`lwip/src/include/netif/ppp/ppp_opts.h:504`），
   所以板子发出的满 MTU 帧能被接收。这一条是查证过的，不是假设。

## 尚未验证的未知数（上板时逐条验）

| 未知数 | 现在的处理 |
|---|---|
| NuttX 的 pppd 是否愿意被拨 | 默认 S3 被动监听（`ppp_passive=true`）。若 30 秒无会话，日志会提示把 `GATEWAY_PPP_PASSIVE` 改成 n 重烧 |
| DevKitC-1 排针是否引出 GPIO17/18 | 引脚是 Kconfig 项，可换任意空闲脚 |
| 板子排针上 PA20/PA27 是否可用 | 物理前提，需要接线时确认 |
| 板侧 ppp0 是"僵尸"（对端消失后不掉） | 不修，写进日志提示与本文档：S3 重启后板上重跑 pppd |

## 与 `pc_side/` 的关系

两者对板子完全等价，选哪条只看"要不要 PC 在旁边"：

- `pc_side/` 是 PC 当对端，**已在真机端到端验证**（`../README.md` 路线 1/2）
- 这里是 S3 当对端，**软件完成、未上板**

`../pc_side/ppp_e2e.py --mode esp32s3` 会跳过 PC 侧的串口桥与 `ppp_up.sh`，
只负责把板子的 pppd 拉起来并从控制台验证。
