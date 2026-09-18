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
| **PPP 服务端互通（S3 ↔ PC，真机）** | **已验证**（2026-09-18，见"台架实测"一节）：LCP/IPCP 协商成功、地址分配正确、ping 5/5 零丢包 |
| **本地模型（llama2.c，真机）** | **已验证**：stories260K 在 PSRAM 里跑，21–26 tok/s；OpenAI 兼容端点对着 PPP 链路开放 |
| **与板子的端到端（板子问、S3 的本地模型答）** | **已验证**（2026-09-18，三根杜邦线，见"与板子联调"一节）：Agent 的 15.5 KB 请求经 PPP 进 S3，模型生成 576 字符回答，12.4 s 返回；S3 重启后板子 6.9 s 自动重拨 |
| **NAPT 转发（板子经 S3 上公网）** | **已验证**（2026-09-18，2.4 GHz 热点）：板子 ping 223.5.5.5，4/4、0% 丢包、平均 80 ms；期间本地模型端点持续在线。过程逼出一个真实故障并修复（提交 ac6f6fb）：WiFi 驱动的 AMPDU BA 会话 ROM 打印从高优先级任务灌控制台，把 2 KB 的 PPP UART 接收环挤爆（44 ms 余量）→ 已关 AMPDU RX + 环扩到 8 KB，详见台架证据 §七 |
| **云-端自适应路由** | **已验证**（2026-09-18，真云真 key，见台架证据 §八）：云端在线时板子 `ask` 7.5 s 拿到 271 字节连贯回答（中文提问 PC 直连 3.5–4.7 s）；**关热点** → S3 报 `no uplink -- the local model answers (offline mode)`，板子照样答（本地故事体）；**开热点** → 自动回切云端。过程逮住并修复一个 body 截断 bug（提交 c756cb4）——此前云端一路 400、每次静默降级，功能看着可用而云端腿从未生效 |

四项全部真机验证完毕：S3 ↔ PC 互通、板子问/S3 的本地模型答、板子经 S3 上公网、
本地模型 21–26 tok/s。软件与实测已闭环。

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

**一个环境限制要先说**：Windows 的 ESP-IDF 无法在 `\\wsl.localhost\` 共享路径上
构建本工程——IDF 的 CMake 探测编译器时会起一个 `cmd.exe` 子进程，而 cmd 拒绝以
UNC 路径为当前目录，链接器随即报 Permission denied。所以构建要在 Windows 本地
目录里做：把工程拷过去（例如 `C:\Users\<you>\esp32s3_gateway_build`），在副本里
跑上面三步；WSL 里的仓库始终是源真身。本机用的包装脚本 `gateway_run.bat`
（set-target / build / menuconfig / flash COMx）在仓库外，随竞赛交付一并归档。

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

## 与板子联调（2026-09-18，真机通过）

三根杜邦线（TX GPIO17→PA20、RX GPIO18←PA27、GND↔GND），部署固件的台架
变体（PPP 走 UART1，日志留在 UART0）：

```
板子 nsh> pppd /dev/ttyS0 460800 &        # 之前验证过的同一句，零改动
S3   日志: session 1 up: we are 10.0.0.1, board is 10.0.0.2
板子 nsh> ping -c 3 10.0.0.1              # 56 bytes from 10.0.0.1, ~50 ms
```

Agent 那一侧（这就是"协同"的完整形态——对端是另一块自己跑着推理固件的板子）：

```
vela> set_llm http://10.0.0.1/v1 stories260K local
vela> ask One day, a little girl
[llm] Response: 576 bytes text, 0 tool calls, finish=end_turn   (12.4 s)
[Agent]: One day, a little girl named Lily went to the park with her mommy. ...
```

S3 同时刻的日志：`request: 15551 B body` → `answered: 576 chars in 10.0 s
(26.3 tok/s)`。完整日志原文与三个被这次实测逼出来的修复，见
`../../../docs/ESP32-S3网关台架实测证据.md` 第六节。

**自愈**：S3 重烧固件后**不必**在板上重跑 pppd——板侧 pppd 的 persist
6.9 秒自动重拨。早先"S3 重启后板上要重跑 pppd"的提示对这版板侧固件不成立。

## 本地模型（GATEWAY_LOCAL_LLM，默认开）

网关固件里同时跑着一个 llama2.c 小模型（stories260K，0.26M 参数），在
`http://<本机地址>/v1` 提供 OpenAI 兼容端点——`GET /`（状态页）、
`GET /v1/models`、`POST /v1/chat/completions`。它开在**所有有地址的接口**上：
WiFi 起来之前是 PPP 链路（10.0.0.1），WiFi 起来之后两个都能访问。

用途是**断网降级**：板子在拿不到 Wi-Fi 上行时，`set_llm http://10.0.0.1/v1`
仍能得到回答——小、简单，但确实来自另一块板的本地推理。模型与分词器打包在
`llm` 分区（16 MB flash 布局，`partitions.csv`），由
`esp32s3_common/tools/fetch_model.py` 从 hf-mirror 取件打包、
`tools/model.bat COMx` 烧录；推理代码在两个固件共用的
`esp32s3_common/components/espllm`。纯推理实测 21–26 tok/s（-O2，240 MHz，
PSRAM octal 80 MHz）。

要对模型有正确的期待：0.26M 参数只会写通顺的英文小故事，不会听指令——它是
"链路两端都是嵌入式实现"的证明与离线兜底，不是云端模型的替代品。`menuconfig`
里关掉 `GATEWAY_LOCAL_LLM` 即可拿回 ~1 MB PSRAM、纯做路由。

## 云-端自适应路由（GATEWAY_CLOUD_*，key 为空时退化为纯本地）

`tools/set_cloud.py`（或 `set_cloud.bat`，交互不回显）把云端 API key 写进
gitignore 的 `sdkconfig` 后，同一个 `http://10.0.0.1/v1` 端点就变成了**双模型
路由点**。板子永远只认这一个地址，由网关按**连通性**逐请求决定谁作答：

| 请求里的 model 名 | 上行状态 | 谁回答 |
|---|---|---|
| `local` / `stories260K` | 任意 | 本地模型（显式点名） |
| 其它（如 `auto`） | 上行在线 | **云端大模型**（model 字段在途中改写为云端名，key 只加在 S3 侧） |
| 其它（如 `auto`） | 断网 / 云端失败 | **本地模型**（自动降级，日志与状态页都记录） |

要点：

- **策略是连通性，不是假装懂问题**。小模型判断不了"这题难不难"，但网关
  确切知道"互联网在不在"。有网时板子的 Agent 是全功能的（工具调用、中文、
  推理都来自云端真模型）；断网时同一地址、同一句 `ask`，由本地模型优雅接住。
- key 只进 `sdkconfig`；固件把它用于 `Authorization` 头，**不打印、不回显、
  不入日志**。云端 URL/模型名在 `Kconfig.projbuild`（默认即官方 Agent 同款端点）。
- 状态页 `GET /`（浏览器自刷新，手机连同一热点即可打开）实时显示路由统计：
  cloud ok / fail→local / offline→local / direct local，最近一次提问与路线；
  `GET /status` 是同内容的纯文本版。
- 板侧用法不变：`set_llm http://10.0.0.1/v1 auto local`（末位参数只是占位）。
- **演示断网降级**：正常问一轮 → 关热点 → 再问（本地接住）→ 开热点 → 再问
  （云端回来）。**每轮必须换一个新问法**——板子的 Agent 会把相同问题缓存
  （第二次 `latency=0ms` 直接返回旧答案，压根不发请求），问重了看到的是缓存
  而不是路由结果。实测记录见 `../../docs/ESP32-S3网关台架实测证据.md` §八。

## 台架模式：先只验 PPP，不开 WiFi

`GATEWAY_UPLINK=n` 时跳过 WiFi 与 NAPT，只起 PPP 服务端。用处是把两个未知数分开：
**「两边的 PPP 实现能不能谈成」** 与 **「NAT 转不转」**。前者才是整条路线唯一真正
没底的地方——对面是 NuttX 移植的 pppd，不是 Linux 的——而且它不开 WiFi 就能测，
不必先准备凭据。

这个模式下板子只能到本设备、出不了公网；要出网得由 PC 侧或别的设备提供路由。
日志里会明确写出这一点，免得把"本来就没有上行"误判成"NAPT 坏了"。

## 台架实测（2026-09-18，真机通过）

**接法**：S3 的串口就是一块 FT232（USB 桥接 UART0，见下），PC 侧把 COM10 用
`serial_tcp_bridge.py COM10 460800` 桥成 TCP，WSL 里照 `../pc_side/ppp_up.sh`
起 pppd 当客户端。注意地址对调：服务端（S3）占 10.0.0.1，客户端（PC）拿
10.0.0.2，即 `ppp_up.sh "" 5555 10.0.0.2 10.0.0.1 460800`。

**台架固件与部署固件只差 6 行 sdkconfig**（都在 menuconfig 里能改）：

```
CONFIG_GATEWAY_UPLINK=n          # 不开 WiFi/NAPT
CONFIG_GATEWAY_UART_PORT=0       # PPP 走 UART0（FT232 那路）
CONFIG_GATEWAY_UART_TX_GPIO=43
CONFIG_GATEWAY_UART_RX_GPIO=44
CONFIG_ESP_CONSOLE_NONE=y        # 日志输出关掉，UART0 让给 PPP 数据流
                                 # （部署版是 UART_DEFAULT，日志走 UART0）
```

**结果**：

| 项 | 结果 |
|---|---|
| LCP | 协商成功，双向 Echo-Req/Rep 正常 |
| pppd 提出 CCP（压缩） | S3 `Protocol-Rej` → pppd 干净丢弃（对应 `VJ/压缩` 各开关全关） |
| pppd 提出 IPv6CP | S3 `Protocol-Rej`（对应 `LWIP_PPP_ENABLE_IPV6=n`） |
| pppd IPCP 提出 VJ 头压缩 | S3 `ConfRej`（对应 `LWIP_PPP_VJ_HEADER_COMPRESSION=n`，NAPT 的硬性要求） |
| 地址分配 | S3 报自身 10.0.0.1、把 10.0.0.2 分给 PC——两个服务端地址参数都生效 |
| 数据面 | `ping 10.0.0.1`：5 发 5 中，0% 丢包，平均 23 ms |

值得记下的一点：三个"拒绝"恰好逐条对应 `sdkconfig.defaults` 里三个 `=n`——
配置是否真的生效，在对端的协商日志里看得清清楚楚。**NuttX pppd 主动发起、
lwIP 服务端被动应答这条此前唯一没底的互通问题，就此关闭**：
`GATEWAY_PPP_PASSIVE=y`（默认值）是对的，不需要重烧。

**完整证据**（pppd 协商日志原文、复现命令、满 MTU 与丢包统计）在
`../../../docs/ESP32-S3网关台架实测证据.md`。

**为什么台架走 UART0**：这块 S3 的 USB 串口是板载 FT232 接 UART0（GPIO43/44），
即 esptool 能直接烧录的那一路；台架恰好复用它，连第二根线都省了。部署版跑
UART1（GPIO17/18）对接板子，与此不冲突——只是 Kconfig 值不同。

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

## 当初的未知数——逐条验证记录

| 未知数 | 现在的处理 |
|---|---|
| ~~NuttX 的 pppd 是否愿意被拨~~ | **已解决**（台架实测）：主动发起、接受协商，`GATEWAY_PPP_PASSIVE=y` 默认值正确 |
| ~~NAPT 是否正确转发（Wi-Fi 上行 → PPP）~~ | **已解决**（台架实测 §七）：板子 ping 223.5.5.5，4/4、0% 丢包、平均 80 ms |
| DevKitC-1 排针是否引出 GPIO17/18 | **已解决**：已用 GPIO17/18 接线实测 |
| ~~板子排针上 PA20/PA27 是否可用~~ | **已解决**：三根杜邦线已接通并实测 |
| 板侧 ppp0 是"僵尸"（对端消失后不掉） | **不再需要担心**：板侧 pppd 的 persist 会自动重拨（实测 6.9 s）；仅当 pppd 本身被杀时才需手动重启 |

## 与 `pc_side/` 的关系

两者对板子完全等价，选哪条只看"要不要 PC 在旁边"：

- `pc_side/` 是 PC 当对端，**已在真机端到端验证**（`../README.md` 路线 1/2）
- 这里是 S3 当对端，**已全部真机验证**（对 PC 互通 → 板子端到端 → NAPT 上公网）；
  板子侧零固件改动，`set_llm http://10.0.0.1/v1` 即可在云端模型与本地模型间切换

`../pc_side/ppp_e2e.py --mode esp32s3` 会跳过 PC 侧的串口桥与 `ppp_up.sh`，
只负责把板子的 pppd 拉起来并从控制台验证。
