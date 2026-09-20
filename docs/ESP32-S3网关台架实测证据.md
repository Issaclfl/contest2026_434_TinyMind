# ESP32-S3 网关 · 台架实测证据

**日期**：2026-09-18
**目的**：把 3.4.9（ESP32-S3 当 PPP 服务端）里唯一真正没底的那一项单独证掉——
**NuttX 移植的 pppd 与 lwIP 的 PPP 服务端能不能谈成**。这条与板子无关，所以
不需要动板子、也不需要接线：S3 对 PC 侧的 pppd 做客户端。

**这一页证明什么、不证明什么**

| | 状态 |
|---|---|
| 两边的 PPP 实现能否协商成功（LCP/IPCP） | ✅ 本页实测 |
| 服务端地址参数是否真的生效（10.0.0.1 / 10.0.0.2） | ✅ 本页实测 |
| 协议裁剪是否真的生效（CCP / IPv6CP / VJ） | ✅ 本页实测 |
| 链路数据面与满 MTU（1500）通路 | ✅ 本页实测 |
| Wi-Fi 上行 + NAPT 转发 | ❌ 需要 Wi-Fi 凭据，未测 |
| 板子 ↔ S3 的最后一跳 | ❌ 需要三根杜邦线，未测 |

---

## 一、被测固件与接法

S3 烧的是**台架模式**固件——与部署版只差 6 个 Kconfig 值，都在 `menuconfig` 里：

```
CONFIG_GATEWAY_UPLINK=n          # 不开 WiFi/NAPT，只起 PPP 服务端
CONFIG_GATEWAY_UART_PORT=0       # PPP 走 UART0
CONFIG_GATEWAY_UART_TX_GPIO=43
CONFIG_GATEWAY_UART_RX_GPIO=44
CONFIG_ESP_CONSOLE_NONE=y        # 关掉日志输出，把这个口让给 PPP 数据流
```

```
PC ──USB── FT232（板载，接 S3 的 UART0 = GPIO43/44）── ESP32-S3
                 │
                 └─ serial_tcp_bridge.py COM10 460800  →  TCP 5555
                                            ↓
                     WSL: socat 造 PTY → pppd 10.0.0.2:10.0.0.1 → ppp0
```

注意**地址对调**：服务端（S3）占 10.0.0.1，客户端（PC）拿 10.0.0.2——
与 `pc_side/ppp_up.sh` 默认值相反，因为这次当服务端的是 S3。

## 二、复现命令

```bash
# 1) S3：烧台架版（UART0/43/44、UPLINK=n、console none）
tools\build.bat
tools\flash.bat COM10

# 2) Windows：把这个口桥到 TCP
python serial_tcp_bridge.py COM10 460800 --tcp-port 5555

# 3) WSL：起 PPP 客户端，参数为 <宿主> 5555 10.0.0.2 10.0.0.1 460800
wsl.exe -d Ubuntu-24.04 -u root -e bash -lc \
  'bash <repo>/board/sf32lb52_net/pc_side/ppp_up.sh "" 5555 10.0.0.2 10.0.0.1 460800'
```

台架版固件的日志输出是关的（UART0 让给了数据流），所以**证据只有 pppd 这一侧**；
要同时看 S3 的日志，把 `CONFIG_ESP_CONSOLE_NONE` 关掉、PPP 改用 UART1 即可
（那正是部署版的配置）。

## 三、pppd 侧协商日志（原样）

```
using channel 1
Using interface ppp0
Connect: ppp0 <--> /tmp/ttyPPP
sent [LCP ConfReq id=0x1 <asyncmap 0x0> <magic 0x8bb4c704> <pcomp> <accomp>]
rcvd [LCP ConfReq id=0x2 <asyncmap 0x0> <magic 0x3f58a087> <pcomp> <accomp>]
sent [LCP ConfAck id=0x2 <asyncmap 0x0> <magic 0x3f58a087> <pcomp> <accomp>]
rcvd [LCP ConfAck id=0x1 <asyncmap 0x0> <magic 0x8bb4c704> <pcomp> <accomp>]
sent [LCP EchoReq id=0x0 magic=0x8bb4c704]
sent [CCP ConfReq id=0x1 <deflate 15> <deflate(old#) 15> <bsd v1 15>]
sent [IPCP ConfReq id=0x1 <compress VJ 0f 01> <addr 10.0.0.2>]
sent [IPV6CP ConfReq id=0x1 <addr fe80::d9c0:591b:97a3:a338>]
rcvd [IPCP ConfReq id=0x1 <addr 10.0.0.1>]
sent [IPCP ConfAck id=0x1 <addr 10.0.0.1>]
rcvd [LCP EchoRep id=0x0 magic=0x3f58a087]
rcvd [LCP ProtRej id=0x3 80 fd 01 01 00 0f 1a 04 78 00 18 04 78 00 15 03 2f]
Protocol-Reject for 'Compression Control Protocol' (0x80fd) received
rcvd [LCP ProtRej id=0x4 80 57 01 01 00 0e 01 0a d9 c0 59 1b 97 a3 a3 38]
Protocol-Reject for 'IPv6 Control Protocol' (0x8057) received
rcvd [IPCP ConfRej id=0x1 <compress VJ 0f 01>]
sent [IPCP ConfReq id=0x2 <addr 10.0.0.2>]
rcvd [IPCP ConfAck id=0x2 <addr 10.0.0.2>]
Script /etc/ppp/ip-pre-up started (pid 542)
Script /etc/ppp/ip-pre-up finished (pid 542), status = 0x0
local  IP address 10.0.0.2
remote IP address 10.0.0.1
Script /etc/ppp/ip-up started (pid 545)
Script /etc/ppp/ip-up finished (pid 545), status = 0x0
```

**逐行读出来的四条结论**：

1. **LCP 双向协商成功**（第 4–7 行互发 ConfReq/ConfAck），且 Echo 一问一答正常
   （第 8、14 行）——链路层通了，保活也通了。
2. **服务端两个地址参数都真的生效**：S3 报出自己一侧是 10.0.0.1（第 12 行
   `rcvd [IPCP ConfReq <addr 10.0.0.1>]`），并把 10.0.0.2 分给了客户端（第 21 行
   `IPCP ConfAck <addr 10.0.0.2>`）。若 `CONFIG_LWIP_PPP_SERVER_SUPPORT` 没开，
   `esp_netif_ppp_set_params()` 会返回 ESP_OK 却**默默忽略**这两个地址——这正是
   `main/net_ppp.c` 里那道 `#error` 守卫要防的事。
3. **协议裁剪逐条对应配置**：pppd 提出 CCP（压缩）→ `Protocol-Reject`（第 15–16 行）；
   提出 IPv6CP → `Protocol-Reject`（第 17–18 行）；IPCP 里提出 VJ 头压缩 →
   `ConfRej`（第 19 行）。三条分别对应 `sdkconfig.defaults` 里的
   `LWIP_PPP_VJ_HEADER_COMPRESSION=n`、`LWIP_PPP_ENABLE_IPV6=n`，以及未启用
   PAP/CHAP 与压缩。**配置是否真的生效，在对端日志里看得清清楚楚**——这比"编译
   通过"强得多。
4. **发起方向确认**：pppd 是主动方（`sent [LCP ConfReq]` 在先），S3 被动监听即
   应答——默认的 `GATEWAY_PPP_PASSIVE=y` 正确，不需要重烧。

## 四、数据面

```
### ping 10.0.0.1 20 包:
--- 10.0.0.1 ping statistics ---
20 packets transmitted, 20 received, 0% packet loss, time 5718ms
rtt min/avg/max/mdev = 6.330/20.276/34.962/7.641 ms

### 满 MTU（1472+28=1500，禁止分片）:
--- 10.0.0.1 ping statistics ---
3 packets transmitted, 3 received, 0% packet loss, time 2005ms
rtt min/avg/max/mdev = 80.678/87.658/92.762/5.108 ms

### ppp0 计数:
    RX:  bytes packets errors dropped  missed   mcast
          6210      26      0       0       0       0
    TX:  bytes packets errors dropped carrier collsns
          6245      28      0       0       0       0

### 接口:
    inet 10.0.0.2 peer 10.0.0.1/32 scope global ppp0
```

- **20/20 零丢包**，往返 6–35 ms（串口 460800，另有 PC 侧 TCP 桥的一跳）。
- **满 MTU 通路成立**：`ping -M do -s 1472` 即 1472+28=1500 字节整帧、禁分片，
  3/3 通过。这一条直接支撑"MTU 1500 对齐"的设计——板侧链路 MTU 由
  `CONFIG_NET_TUN_PKTSIZE` 固定 1500，lwIP 的 `PPP_MRU` 默认也是 1500
  （`netif/ppp/ppp_opts.h:504`），所以一次 TLS 握手不会被切成二十个包。
- 两个方向的 `errors`/`dropped` 全为 0。

## 五、这次测试的边界（不夸大）

- 台架模式**没有 Wi-Fi、没有 NAPT**，所以本页**不能**证明板子能上公网。
- 本页证明的是两端 PPP 实现的互通性——这是整条路线里最贵、最不确定的一环；
  剩下两项（NAPT 转发、板子接线）出问题时，现在可以确定地排除掉它。
- 台架版与部署版只差上面那 6 行 Kconfig，`main/` 下**一行代码都没动**。

---

## 六、第二阶段：板子接上三根线之后的端到端（2026-09-18）

第一节的对端是 PC；这一节把对端换成板子本身。三根杜邦线（S3 GPIO17→PA20、
GPIO18←PA27、GND↔GND），S3 仍是台架配置（PPP 走 UART1/GPIO17-18，日志在
UART0/COM10，无 WiFi——**本地模型不需要 WiFi**）。

### 上电自愈（先记一件没预料到的好事）

S3 重烧固件后**没有**在板子上重跑 pppd：板侧 pppd 的 persist 自动重拨，
6.9 秒后 S3 报 `session 1 up`。README 里"S3 重启后板上要重跑 pppd"的提示
对**这版板侧固件**不成立，已过时。

### S3 侧日志（原样，节选）

```
I (819) gw_ppp: listening on UART1 (TX GPIO17, RX GPIO18) at 460800 baud, passive
I (949) espllm.model: 1062783 B model copied to PSRAM (8380296 B were free)
I (979) espllm: model: 5 layers, dim 64, hidden 172, 8 heads (4 kv), vocab 512, seq_len 512
I (989) espllm.http: openai-compatible endpoint listening on port 80 (GET /, GET /v1/models, POST /v1/chat/completions)
I (8869) gw_ppp: session 1 up: we are 10.0.0.1, board is 10.0.0.2
I (8879) gw_ppp: session running
I (37849) espllm.http: request: 15551 B body, prompt (22 chars): One day, a little girl
I (47889) espllm.http: answered: 576 chars in 10.0 s (26.3 tok/s)
```

### 板子侧（Agent 在 vela> 提示符里）

```
[netmgr] Found iface ppp0 addr 10.0.0.2
[agent] Network connected: 10.0.0.2
[agent] All network services started!

vela> set_llm http://10.0.0.1/v1 stories260K local
LLM backend: 10.0.0.1:80/v1/chat/completions (model: stories260K)

vela> ask One day, a little girl
[llm] OpenAI API with tools (model: stories260K, 15551 bytes)
[llm] Response: 576 bytes text, 0 tool calls, finish=end_turn
[trace] iter=0 tool=(none) latency=12402ms llm=ok backend=0
[Agent]: One day, a little girl named Lily went to the park with her mommy.
They saw a lot of blocks in the forest. ...
```

### 这一节证明什么

- **"板子问、S3 答"全链路**：Agent 的 OpenAI 请求（系统提示+工具表 15.5 KB）
  经三根线上的 PPP 进入 S3，S3 上的 llama2.c 模型生成 576 字符回答，原路返回
  并显示在板子上。**全程无 PC 参与（PC 只当两个串口的看客）、无互联网。**
- 板子的 Agent 把这条串口链路当成自己的网络（`Network connected: 10.0.0.2`）。
- 链路自愈：S3 重启后板子 6.9 秒自动重拨。

### 这一次实测逼出来的三个修复（都已入库，提交 61950a0）

1. **HTTP 只 recv 一次**：15.5 KB 请求在 MTU 1500 的 PPP 上拆成十几段，一次
   recv 只拿前几段，JSON 残缺、字符串没有闭合引号 → 连续三个 400。改成循环
   读满 Content-Length。
2. **KV 缓存越界隐患**：封装里 steps 可超 seq_len，上游 forward() 用 pos 直接
   索引 KV 缓存且无边界检查。改成先数 prompt token 再封顶。
3. **PPP 监控任务时间单位错误**（秒算成百秒，提示 1 秒就喊、重试一秒一次）+
   对未建立过的监听器重复 re-arm。另把 ESTABLISH 阶段日志改为如实的"协商中"。

## 七、第三阶段：板子经 S3 的 WiFi 上公网（NAPT，2026-09-18）

第六节的对端链路里 S3 只当"离线模型"；这一节把 S3 的另一半职责——
**WiFi 出口 + NAPT**——也验证掉。至此三块板的角色完全定型：

```
SF32LB52（Agent/音频） --三根杜邦线 PPP--> ESP32-S3（本地模型 + NAPT） --WiFi--> 互联网
```

### 先记录这次逼出来的失败：AMPDU ROM 打印饿死 PPP

WiFi 关联成功后 PPP 反而掉线（`peer asked to close the session`）。根因不在
PPP：WiFi 驱动每建立一条 AMPDU BA 会话，就从它的高优先级任务直接往控制台
打一行 ROM 日志（无标签、不受日志级别控制）。手机热点的 BA 会话建立期这行
打印连环出现，把 460800 波特率下只有 44 ms 余量的 2 KB UART 接收环挤爆，
PPP 帧在环里被覆写——UART 没有 PPP 那样的重传，链路就死了。

修复（提交 ac6f6fb）：

1. `CONFIG_ESP_WIFI_AMPDU_RX_ENABLED=n`——460800 波特率的 PPP 上 AMPDU
   接收毫无收益，从源头消灭这批打印；
2. PPP 接收环 2048 → 8192 B（约 170 ms 余量），防的是下一次别的什么洪泛；
3. 顺带加了诊断：NO_AP_FOUND 时扫描并只报告 AP 总数 / 2.4 GHz 数 / 配置的
   SSID 是否可见（信道+信号），从不打印邻居名字。

（第一次连不上热点是另一回事：SSID 是 5 GHz。切 2.4 GHz 后关联成功。）

### 实测（原样，节选）

S3 侧：`uplink ready: 10.138.138.122`（WiFi 关联、拿到地址），随后板子拨入：

```
I (....) gw_ppp: session 1 up: we are 10.0.0.1, board is 10.0.0.2
```

板子侧（WiFi 全程在线，S3 同时在服务本地模型端点）：

```
nsh> ifconfig
ppp0	Link encap:TUN at RUNNING mtu 1500
	inet addr:10.0.0.2 DRaddr:10.0.0.1 Mask:0.0.0.0

nsh> ping -c 4 223.5.5.5
PING 223.5.5.5 56 bytes of data
56 bytes from 223.5.5.5: icmp_seq=0 time=100.0 ms
56 bytes from 223.5.5.5: icmp_seq=1 time=70.0 ms
56 bytes from 223.5.5.5: icmp_seq=2 time=80.0 ms
56 bytes from 223.5.5.5: icmp_seq=3 time=70.0 ms
4 packets transmitted, 4 received, 0% packet loss, time 4050 ms
rtt min/avg/max/mdev = 70.000/80.000/100.000/12.247 ms
```

### 这一节证明什么

- **NAPT 转发**：板子（10.0.0.2）发出的 ICMP 经 PPP 到 S3，被 NAPT 翻译成
  WiFi 侧（10.138.138.122）的报文上公网，回程原路返回——4/4、0% 丢包。
- **S3 双职责同时在线**：上公网的同时，本地模型 HTTP 端点继续在 PPP 侧服务。
  云端模型与本地模型对板子是同一个地址的两种后端。
- **PC 彻底出局**：上一节去掉的是"模型依赖 PC"，这一节去掉的是"上网依赖
  PC"。整套系统（板子 + S3 + 热点）不再需要一台电脑。
- 修复在真实故障上验证：同一配置在 AMPDU 关闭前 30 秒内必然掉线，关闭后
  会话稳定、公网往返正常。

### 补记：全栈同刻（WiFi/NAPT 在线 + 本地模型服务）

上表的 ping 之外，又做了一次叠加态验证——WiFi 与 NAPT 保持在线的同时，
板子 Agent 走 `set_llm http://10.0.0.1/v1 stories260K` 向 S3 本地模型提问：

```
[netmgr] Found iface ppp0 addr 10.0.0.2
[agent] Network connected: 10.0.0.2
vela> set_llm http://10.0.0.1/v1 stories260K local
LLM backend: 10.0.0.1:80/v1/chat/completions (model: stories260K) [router slot 0]
vela> ask One day, a little girl
[Agent]: One day, a little girl named Lily went to the park with her mommy.
They saw a lot of blocks in the forest. ...
```

回答正常生成并显示。至此**同刻在线的三件事**：板子经 S3 上公网（NAPT）、
S3 对板子服务本地模型、S3 自己的 WiFi 上行——一台 S3 同时是"路由器"和
"推理机"，板子侧零固件改动。

## 八、第四阶段：云-端自适应路由（2026-09-18，真云真 key）

### 它是什么

同一个 `http://10.0.0.1/v1` 端点，板子侧零改动；网关按**连通性**逐请求决定谁作答：

| 请求的 model 名 | 上行状态 | 谁回答 |
|---|---|---|
| `local` / `stories260K` | 任意 | 本地模型（显式点名） |
| 其它（如 `auto`） | 在线 | **云端大模型**（model 字段在途中改写，key 只加在 S3 侧） |
| 其它（如 `auto`） | 断网 / 云端失败 | **本地模型**（自动降级） |

策略刻意选"互联网在不在"而不是"判断题目难不难"——0.26M 的小模型没有判断能力，
网关却确切知道上行通不通。两条通道都是真模型，没有假装。

### 实测中逮到的一个真 bug（已修，提交 c756cb4）

第一次跑云端腿，每次都"成功"返回答案——但答案全是本地模型的故事体，账本里
`cloud ok` 一直是 0。原因：改写模型名会让 body 变长（`"auto"` → `"mimo-v2.5-pro"`），
而发送时沿用了原始长度，JSON 被截掉 9 字节，云端一路 400 Bad Request，
固件静默降级回本地。**功能看起来能用，云端腿其实从未生效**——这正是"必须真跑
一遍"的价值。修复后日志给出铁证：

```
I (17026) gw_cloud: model "auto" -> "mimo-v2.5-pro" (79 -> 88 B body)
I (17166) esp-x509-crt-bundle: Certificate validated
I (20536) gw_cloud: cloud answered in 3.5 s
```

### 三段实测（同一块板、同一个地址、同一个问题）

**① 云端在线**：板子 `ask "What is edge computing?..."` →

```
[llm] Response: 271 bytes text, 0 tool calls, finish=end_turn
[trace] iter=0 tool=(none) latency=7461ms llm=ok backend=0
[Agent]: Edge computing is a distributed computing paradigm that brings data
processing and storage closer to the sources of data... enables real-time
processing for IoT devices and applications.
```

271 字节日志里那句连贯回答，0.26M 的本地模型不可能产出。PC 侧直连的对照
实测：中文提问 → 云端 3.5–4.7 s 应答（连续三发 5.8/4.7/3.9 s 全过）。

**② 断网降级**：手机关热点 → S3 连报 12 次 `uplink lost (reason 201)`
（NO_AP_FOUND，即热点本身消失了）→ 此时用板子问**同一个问题**：

```
W (354526) gw_cloud: no uplink -- the local model answers (offline mode)
```

板子照样收到回答，但换成本地模型的故事体：

```
[Agent]: What is edge computing? Daddy is always just a party. One day,
Kitty's mom bought him a ground home with a toys. ...
```

——**降级不等于失能**：链路、Agent、问答流程全活着，只是答案质量诚实地差下去。
账本记 `offline->local: 1`。

**③ 恢复回切**：热点回来 →

```
I (405506) gw_wifi: uplink ready: 10.138.138.122, gateway 10.138.138.25
```

板子问一个新问题 → `latency=7542ms`、230 字节、云端连贯回答（"A microcontroller
is a compact integrated circuit..."），账本回 `cloud ok`。

（另有一条确定性失败注入：请求不带 model 字段 → 云端必 400 → 本地接住，
账本 `fail->local: 1`。降级的"失败"侧与"断网"侧都验证到了。）

### 这一节证明什么

- **S3 从"网络出口"升级为"智能出口"**：有网时它是云端大模型的中继（板子侧
  零改动、key 不下发板子），断网时它就是模型本身。板子永远只知道一个地址。
- **降级是自动的、秒级的**：断网判定走前置检查（不等超时），板子上看不到任何
  卡顿或报错。
- **两个真实故障被实测逮住并修复**：body 截断（上面）与 AMPDU 打印饿死 PPP
  （§七）。这类缺陷编译期全不可见。

### 三个操作层面的坑（值得写下来）

1. **板子会把相同问题缓存**：连问两次同一句话，第二次 `latency=0ms` 直接返回
   缓存答案，压根不发请求。演示时**每轮换一个新问法**，否则看到的是缓存而不是
   路由结果。
2. **PC 上关串口 = 按 S3 的复位键**：这块 S3 的 FT232 把 RTS 接到了 EN（`--reset`
   就是利用这一点）。任何串口脚本退出/关闭端口，ESP32 就会复位一次——排查时
   别把它当成"固件崩溃"。相应地，复位后板子的 `ppp0` 可能变"僵尸"（还挂着旧
   地址但没有会话），修法是板上 `kill` 掉 pppd 再 `pppd /dev/ttyS0 460800 &`，
   或让 S3 完整复位一次重新协商。
3. **在板子上退出 agent（`vela>` 里敲 `quit`）之后，那块板子的控制台就死了**——这是
   2026-09-19 实测到的，不是在讲道理：`quit` 打完 `Exiting agent... [cli] CLI thread
   exiting` 之后，回车、`ifconfig`、`help` 全部无回显（连提示符都没有），8 秒长监听
   零字节；软件侧试过 RTS/DTR 的各种极性组合与关开端口，都叫不回来。这类彻底静默
   和文档里记过的"断言 `while(1)` 静默死循环"是同一个观感：**没有日志、没有复位**。
   恢复方式只有物理动作——按板载 Reset 或拔插 USB。
   **所以重拨 pppd 不要走 `quit`**：板子断电重启 → 到 `nsh>` → `pppd /dev/ttyS0 460800 &`
   → `ai_agent`。（本次会话里板子就停在这个状态等用户复位，S3 侧的验证不受影响——
   语音与命令那条链路根本不经过板子。）

### 补记：云端模型名不是常量，而这次排查正好验证了降级链路

（2026-09-19）昨天的实测用的是官方同款 `mimo-v2.5-pro`。今天再问时，板子拿到的是
**本地小模型的回答**（故事味），而状态页 `route` 行写着 `0 cloud ok / 2 fail->local`
——云端腿每次都失败、降级在工作。S3 日志给出原因：

    I (23927) gw_cloud: model "auto" -> "mimo-v2.5-pro" (16176 -> 16185 B body)
    W (24747) gw_cloud: cloud answered HTTP 500: {"error":{"code":"500","message":"Internal Server Error",...}}
    W (24747) gw_cloud: falling back to the local model

从 PC 直连同一个端点（`源码对照/cloud_probe.py`，key 从 sdkconfig 读、脚本只打印
状态码）同样是 500，且**与请求大小无关**（120 B 与 16 KB 都 500），而
`GET /v1/models` 返回 200 并列出 `mimo-v2.5` / `mimo-v2.5-pro` / `-tts` / `-asr`
等。换成 `mimo-v2.5` 后立刻正常：

    [trace:7c5432a9...] iter=0 tool=(none) latency=11665ms llm=ok backend=0
    [Agent]: A microcontroller is a small, compact integrated circuit that contains a
             processor, memory, and input/output peripherals, designed to control
             specific functions in embedded systems like appliances, cars, medical
             devices, and IoT gadgets.

`route` 行同时变成 `1 cloud ok`。结论：**不是固件的问题，是服务端对某个模型名
不可用**。而这次排查顺带证明了降级链路是可信的——云端 500 时板上不报错、不卡顿，
只是回答换了个来源，且状态页与日志都留下了痕迹。模型名可配（`set_cloud.bat` 里填），
所以这类问题不需要改代码。

## 九、权重量化：自己写的量化器 + 设备侧量化矩阵乘

**状态：PC 主机侧与设备侧都已实测完成（见 9.4 与 9.6）。**

### 9.1 为什么做——不是"为了好看"，是带宽

台架上 S3 的本地模型（stories260K）实测 21–26 tok/s，这个数字对不上算力：
0.26M 参数的 float 乘加在 240 MHz 上该有几百 tok/s。对得上的是**带宽**：每生成
一个 token 要把全部权重读一遍，fp32 就是 1.04 MB/token；43 ms 一个 token
→ 24 MB/s 的有效带宽，正好是 PSRAM 上按行扫描的实际水平。今天的开机日志印证了
权重确实在 PSRAM 里（不是 flash 慢的问题，是 PSRAM 带宽的问题）：

    I (6916) espllm.model: 1062783 B model copied to PSRAM (8374152 B were free)

量化在 S3 上的意义因此不是"省空间"（8 MB 的 llm 分区装得下 1 MB 模型），而是
**每个 token 要读的字节数**：int8 读 1/4，int4 读 1/8。

（这一段是**动手前的推断**。设备实测把它校正了，见 9.6：量化后的内核是算力受限、
不是带宽受限，所以字节少了 3.8× 而每个 token 只省了 20%。推断留在文里，因为它
解释了当初为什么去做量化；校正结果写在 9.6。）

### 9.2 量化器：逐行对称，刻意不用 group

llama2.c 的 runq.c / llama.cpp 用"分组"量化，组大小必须整除每个被量化的维度。
stories260K 的 hidden_dim = 172 既不是 32 也不是 64 的倍数，尾巴会被留在量化之外。

我们的做法（`esp32s3_common/tools/quantize_model.py`，纯标准库 Python，不依赖
numpy）：

    一行 = 一个输出神经元的全部权重，共用一个 fp32 缩放因子
        int8: s = max|x| / 127,  q ∈ [-127, 127]
        int4: s = max|x| / 7,    q ∈ [-8, 7]（q+8 塞进半字节，低半字节在前）
    激活向量整条量化成一个 int8 向量（一个标量缩放因子）
    点积在 int32 上累加（最坏 127×127×512 = 8.3e6，int32 里绰绰有余），
    最后乘回 xs * w_s[i]

好处：**没有任何维度整除约束**，缩放因子开销可忽略（每行 64~172 个权重才 4 B）。
打包头第 12~15 字节原先的 4 个保留字节成了格式字节（0 = 上游 fp32、1 = int8、
2 = int4），于是量化包和 fp32 包走同一套 flash 流程，固件按这个字节挑内核。

### 9.3 数字

    stories260K: 5 层, dim 64, hidden 172, 8 head (4 kv), vocab 512, seq_len 512
    待量化参数 259,328 个

    | 包 | 模型字节数 | 相对 fp32 | 逐张量相对 RMS 误差 |
    | fp32 | 1,056,540 | 1.00× | — |
    | int8 |   276,220 | **3.82× 小** | 最大 0.00695（w2），最小 0.00505（wk） |
    | int4 |   146,556 | **7.21× 小** | 最大 0.12597（w2），最小 0.09238（wk） |

### 9.4 验证：把同一份引擎源码编到 PC 上

量化最容易出的错是"能跑、但答案变差"——**精度损失和实现错误长得一模一样**。
所以先把它们分开：`components/espllm/espllm_engine.c` 不依赖 ESP-IDF，可以原样
编到 PC 上（`tools/host_check.c`）对着同一份包跑同一段 prompt。

1. **打包器核对**：每个张量第一行的缩放因子 == fp32 该行 `max|x|/127`，逐个对上：
   tok 0.00693554、wq 0.00241668、wk 0.00170858、wv 0.00121684、wo 0.00072016、
   w1 0.00228914、w2 0.00221745、w3 0.00179234。
2. **往返核对**（`--roundtrip`）：把量化包还原成 fp32 包（**权重完全一样**，只是改
   走已验证过的 fp32 内核），两者逐位置一致率 90%（int8）/ 90%（int4）——说明
   量化内核是忠实的，不是"实现歪了"。
3. **误差归因**（同 prompt、贪心、逐位置比对下一个 token；`tools/quant_agreement.py`）：

    | 比较 | 差别只在 | 一致率 |
    |---|---|---|
    | fp32 vs q8 还原包 | 权重精度（走 fp32 内核） | 9/10 |
    | q8 vs q8 还原包 | 内核（权重相同） | 9/10 |
    | fp32 vs q8 | 两者 | 13/15 |
    | fp32 vs q4 还原包 | 权重精度 | 6/10 |
    | q4 vs q4 还原包 | 内核（权重相同） | 9/10 |
    | fp32 vs q4 | 两者 | 6/10 |

   读法：**int8 的权重误差已经足以让贪心路径在约 1/10 的位置翻转 argmax**，内核
   自己再贡献同一量级；int4 由权重误差主导（4/10）。两者不是相加关系（翻转点不
   独立）。文本层面三种格式都还是通顺英文：

    - fp32：`Once upon a time, there was a little robot named Benny. Benny loved to
      play with his toys and run around. One day, Benny saw a big, red ball.`
    - int8：`… named Benny. Benny loved to play with his toys and run around. One
      day, Benny's mommy told him to be careful and not go on an adventure.`
    - int4：`… named Benny. Benny loved to play in the sunny day. One day, Benny saw
      a big bird with a small bird.`

4. **一个真实的 bug（值得写下来）**：第一版把层索引乘成了整张量的行数
   （`l * rows`，而 `rows` 已经包含所有层），后果是**第 0 层完全正常、第 1 层起
   全是 NaN，贪心退化成一直输出 `<unk>`**。编译期、精度指标都看不见这种错，只有
   分层打印激活值能抓到。这也是为什么验证顺序是"先核对打包器、再核对内核"。

### 9.5 复现（PC 侧，已跑通）

    cd board/sf32lb52_net/esp32s3_common
    python3 tools/quantize_model.py --format q8 --report          # -> assets/llm_q8.bin
    python3 tools/quantize_model.py --format q4 --roundtrip assets/llm_q4_rt.bin
    gcc -O2 -o ~/qc/host_check tools/host_check.c \
        components/espllm/espllm_engine.c -Icomponents/espllm/include -lm
    python3 tools/quant_agreement.py ~/qc/host_check assets/llm.bin assets/llm_q8.bin

### 9.6 设备实测（S3 N16R8，240 MHz，PSRAM octal 80 MHz）

同一段 prompt、贪心（temperature 0）、80 token、模型整份拷进 PSRAM（日志
`copied to PSRAM`）、三种包背靠背测——同一次会话、同一固件，所以彼此可比：

    | 权重格式 | 每 token 权重字节数 | tok/s | ms/token | 相对 fp32 | 模型体积 |
    | fp32 | 1,056,540 | 39.2 | 25.5 | 1.00× | 1,062,783 B 包 |
    | int8 |   276,220 | 48.2 | 20.7 | **1.23×** |   282,463 B 包 |
    | int4 |   146,556 | 45.3 | 22.0 | **1.16×** |   152,799 B 包 |

**实测把动手前的推断校正了，这一点比数字本身重要。** 9.1 推断"S3 上瓶颈是 PSRAM
带宽"；如果成立，int8 该快 3~4×、int4 该快 6~8×。实测只有 1.23× / 1.16×，而且
**int4 比 int8 还慢一点**——字节数少一半，速度反而降了。这只能说明量化后的内核是
**算力/指令受限**：

- fp32：1.04 MB/token ÷ 25.5 ms = 41 MB/s，这个数确实落在 PSRAM 连续扫描的实际
  水平上，所以 fp32 那 25.5 ms 是带宽受限的；
- int8：0.28 MB/token 若仍按 41 MB/s 该是 6.9 ms，实测 20.7 ms——多出来的十几
  毫秒只能是内核自己（逐字节符号扩展 + int32 乘加，外加每个矩阵乘一次激活量化）；
- int4：字节再减半却更慢，说明 nibble 拆包的开销盖过了省下的带宽。

结论：**在这块板子上"再压字节"已经没有收益了，下一步是让点积快起来**——ESP-DSP
的 PIE/SIMD 点积，或者把两个权重拼进一次 int16 乘加。这是 P4 该做的事，也是这次
量化最有用的产出：把"该优化哪里"从猜测变成了测量。

另记一个操作细节：`GET /status` 的 `weights:` 一行足以确认设备当前跑的是哪个内核，
不需要看日志（换包之后我正是靠它确认 int8 包真的生效了）。

### 9.6.1 设备与 PC 的逐字符一致性（同 prompt、贪心、80 token）

把设备输出与 PC 上同一份 `espllm_engine.c` 的输出逐字符比对：

    | 包 | 设备输出 | PC 输出 | 比对结果 |
    | fp32 | 225 字符 | 225 字符 | **完全一致** |
    | int8 | 222 字符 | 222 字符 | **完全一致** |
    | int4 | 221 字符 | 226 字符 | 前 201 字符一致，之后一次 argmax 翻转 |

fp32 与 int8 逐字符一致，说明设备内核与 PC 内核在做同一件数值事情；int4 在第 201
字符处分岔（x86 与 Xtensa 的浮点细节差异足以翻掉一次 argmax，而 int4 的权重误差
本来就更大）。三种格式的设备侧文本都与 PC 侧对得上，同时说明烧进去的包、格式字节、
内核映射这一整条链路都对。

复现：

    # 设备侧（源码对照/quant_bench.py，本地工具）
    python quant_bench.py <S3 的 WiFi 地址> q8 --save dev_q8.txt
    # PC 侧
    ~/qc/host_check assets/llm_q8.bin "Once upon a time, there was a little robot" 80 0

## 十、命令模型：自己训、自己量化的"能控制硬件"的小模型

前面九节量化的都是**讲故事**的模型——它答得好听，但**不听指令**（0.26M 参数的
童话续写模型，问它什么都在讲 Benny 的故事）。这一节做的是另一件事：让板子上的
小模型真的**驱动硬件**。

### 10.1 先找现成的：找过，走不通（记录在案）

| 项目 | 情况 | 为什么用不上 |
|---|---|---|
| `cactus-compute/needle` | 26M→121M 参数，2.125 bit 的 `.cact` **私有格式** + 自带运行时；二进制默认带 telemetry | 没有 llama2.c 转换路径；29M 参数起步也远超本板可用的 ~6 MB PSRAM |
| `memovai/mimimodel` | 45M 参数、2-bit、ESP32 上跑 | 同样是自己的格式与运行时 |
| `zevorn/rt-claw` | ESP32-C3/S3 上 30+ 工具的真函数调用 | 模型走云端 API，不是端侧 |
| TinyAgent（EMNLP 2024） | TinyLlama-1.1B 微调做设备控制，思路与我们一致 | 1.1B 参数，本板放不下 |
| HuggingFace 上的 llama2.c 格式模型 | 只有 TinyStories 的讲故事/普通指令版 | 都不会输出结构化动作 |

（附注：HF 的 API 从本机这条手机热点访问不通，返回的不是 JSON，所以上表结论来自
网页检索与各仓库页面，而不是 API 列表。）

结论：要在这块板子上"让模型控制硬件"，**必须自己训一个**——这正好也是题目要求的
"自己量化"。而上表也确认了做法本身可行：小模型 + 闭集动作 + 语法约束，是这类
项目的共同套路。

### 10.2 三段流水线（全部在本仓库里，可复现）

1. **数据** `tools/make_cmd_dataset.py`：把动作空间做成**闭集**（颜色 6 种、
   wifi_scan、ping 的 3 个目标、status），再按"动词族"组合生成英文说法。
   **留出集整族留出**，所以测的是"换个说法还认不认"，不是"背没背下来"。
   330 条训练 / 46 条留出。
2. **训练** `tools/train_cmd_model.py`：从 stories260K 微调（torch CPU，**5 分钟**）。
   只对答案部分算损失；序列以 BOS 收尾——板子上的 `generate()` 采样到 BOS 就停，
   模型因此学会"说完 JSON 就闭嘴"。分词用 `tools/cmd_model_bpe.py`（上游
   encode/decode 的 Python 版，逐字节对齐），这样训练与推理喂的是同一个分词结果。
3. **执行** `main/cmd_exec.c`：模型只输出闭集 JSON，**动作由确定性代码做**
   （颜色→引脚、target 名字→IP+端口）。理解归模型，映射归代码——这是这个规模
   唯一可靠的分工。

### 10.3 数字：三个实现给出同一个答案

| 打分方式 | 留出集完全匹配 |
|---|---|
| torch（训练框架内） | 43/46 = 93.5% |
| PC 上编同一份 `espllm_engine.c`（`tools/score_cmd_model.py`） | 43/46 = 93.5% |
| **int8 量化后**，同一个 PC 打分器 | 43/46 = 93.5% |
| **真机**（HTTP 打 S3，`源码对照/cmd_device_test.py`） | 43/46 = 93.5% |
| 微调前同一批 | 0/12 |

训练集 330/330。**量化在这个任务上没有任何损失**——注意这和第九节"讲故事"
模型的结论不同（那里 int8 会让 13% 的位置翻 argmax）：命令任务的目标是闭集里
的一个选项，容错空间比自由生成大得多。这条对比本身值得记下来。

速度：从口令到动作 **1.2 s**（S3 侧）、板子经 PPP 问到回执 **3.6 s**。

### 10.4 设备侧动作实测（原样）

    turn on the red light      -> {"action":"led","color":"red","result":"led set"}
    switch the blue light on   -> {"action":"led","color":"blue","result":"led set"}   ← 留出集说法
    turn on the white light    -> {"action":"led","color":"white","result":"led set"}
    scan the wifi networks     -> {"action":"wifi_scan","aps":33}
    ping the gateway           -> {"action":"ping","target":"gateway","ip":"127.0.0.1","port":80,"rtt_ms":1,...}
    ping the internet          -> {"action":"ping","target":"internet","ip":"223.5.5.5","port":53,"rtt_ms":48,...}
    ping the board             -> {"action":"ping","target":"board","ip":"10.0.0.2","port":28789,"rtt_ms":18,...}
    what is your status        -> 状态行（路由统计 / 上行 / 板子会话 / RSSI）

板子那一侧的完整链路也跑通了（板子控制台原文）：

    vela> set_llm http://10.0.0.1/v1 cmd local
    LLM backend: 10.0.0.1:80/v1/chat/completions (model: cmd) [router slot 0]
    vela> ask turn on the yellow light
    [trace:...] iter=0 tool=(none) latency=3589ms llm=ok backend=0
    [Agent]: {"action":"led","color":"yellow","result":"led set"}

S3 侧日志（开机与执行）：

    I (6832) gw_cmd: onboard RGB led on GPIO48 (ws2812 via RMT)
    I (7162) gw_cloud: cmd: turn on the green light please
    I (8362) gw_cmd: led green -> led set
    I (8362) gw_cloud: cmd answered in 1.2 s: {"action":"led","color":"green","result":"led set"}

### 10.5 三个诚实的限制

1. **0.26M 参数只能记模式**。留出集错的那 3 条全在"bare"族（`led off`、`red please`、
   `green please`），其中两条把颜色词当成了别的意图。它不是一个通用助手，是一个
   **封闭命令集的翻译器**；要加动作或加说法，就扩数据集重训（5 分钟的事）。
2. **ping 动作测的是到目标端口的 TCP 连接时延**，不是 ICMP：IDF 5.5 的
   `esp_ping.h` 已经变成兼容壳，真正的 API 挪到了不导出的目录，为它单引一个组件
   不划算。返回里 `method` 字段如实写着 `tcp_connect`，不假装是 ICMP。
3. **板载 RGB 灯的引脚随板子版本不同**（rev1.1 是 GPIO48，早期版本 38），
   menuconfig 里可改；灯没有回读，所以 `result:"led set"` 只表示**写成功了**，
   是否真的亮着要人看。固件初始化时会打印用的是哪个 GPIO。

### 10.6 第二批动作：读传感器、取网络数据、转云端问答（2026-09-19）

前四类动作（灯/扫描/ping/状态）都只在这块板子上干活。这一批要说明的是**它不止会点灯**，
而且三类"活"的边界是清楚的：

| 口令 | 动作 | 谁干活 | 实测 |
|---|---|---|---|
| blink the blue light 3 times | `{"action":"led","color":"blue","effect":"blink","times":3}` | RMT 闪灯（闪完停在亮着） | 3.3 s |
| make the green light breathe | `{"action":"led","color":"green","effect":"breath"}` | 呼吸一轮约 2 秒 | 4.0 s |
| what is the chip temperature | `{"action":"temperature"}` | **片内温度传感器**（真读数） | 1.4 s，48.3 °C |
| what time is it | `{"action":"time"}` | SNTP；拿不到就退回云端响应的 `Date` 头 | 1.4 s，2026-09-19 13:27:01（UTC+8） |
| what is the weather in shanghai | `{"action":"weather","city":"shanghai"}` | **wttr.in 的真实数据**（免 key，八城闭集） | 3.5 s，`Shanghai: 🌦️  +30°C` |
| tell me a joke | `{"action":"ask"}` | 本地小模型只判断"该问云端"，原话转给 mimo | 9.4~9.9 s |

语音那条腿同样走通（都是 16 kHz 合成音打 `/voice`）：`What is the chip temperature?`
→ 4.0 s → 47.3 °C；`Tell me a joke.` → 9.4 s → 云端给的笑话，emoji 完整。

**数据集与分数**（同一个 0.26M 模型，动作空间从 4 类扩到 10 类、47 个不同 JSON 串）：

| | 第一批（4 类） | 第二批（10 类） |
|---|---|---|
| 训练 / 留出 | 330 / 46 | 945 / 102 |
| 留出集 | 43/46 = 93.5%（整族留出） | **89/102 = 87.3%** |
| 训练集 | 330/330 | **945/945 = 100%** |

留出方式要说清楚：第一批是**整族留出**（`bare`/`switch` 两族一句都不给模型）；这批新增
的动作一族只有一种活，没法整族留出，所以留的是**每组最后一个说法**——这比整族留出弱一
档。错的 13 条里有 5 条是同一个：`fade the X light in and out`（我造的怪说法，模型认不出
是呼吸，退化成常亮）；其余零散错在 bare 族的 `off` / `green light on`、口语的
`what o clock is it`、`how warm is the chip running`。

**这一批量产里逮到并修掉的五个真问题**（每个都在对应文件里留了注释）：

1. **`espllm_json_field()` 只认字符串值**，而 `"times":3` 是数字 → 闪灯永远只闪一下，
   而且不报错。加了 `json_int_field()` 自己扫数字。
2. **云端用代理对（CESU-8）发 emoji**：🐔 的字节是 `ED A0 BD ED B0 94`——单个 3 字节
   序列看着合法、整体却是非法 UTF-8，浏览器那侧 `json.load` 直接报
   `invalid continuation byte`。修法是 `espllm_utf8_sanitize()`：成对的代理还原成 4 字节
   UTF-8（emoji 留住），落单的丢掉。
3. **`esp_http_client_get_header()` 读不到响应头**（它只认自己 `set` 过的请求头），
   所以"用 Date 头对时"这条腿一开始是坏的、静默返回 -1。改用 `HTTP_EVENT_ON_HEADER`
   事件接响应头。
4. **手机热点不回 UDP 123**：SNTP 等不到答案（IDF 默认还有最多 5 秒的启动延迟，已在
   `sdkconfig.defaults` 里关掉）。所以时间动作是"SNTP 只等 1.5 秒 → 立刻退回 Date 头"，
   第一次等不到就不再等 SNTP。
5. **ASR 把数字转写成词而不是数字**（实测 `blink the blue light three times` 是逐字
   转写），而第一批只训了 `3 times` → **语音说这句会翻车**（打字没这个问题，真机上才
   暴露）。补了词形（two/three/four/five times）重训后，语音与打字两条路都对。

**边界（诚实）**：
- `ask` 需要联网 + key：没有上行时它如实回 `no uplink -- answering a question needs the
  internet`，不编答案；延迟 9~10 秒，因为云端那个模型是思考型，要先"想"。
- `time` / `weather` 同样依赖外网；`weather` 用的是第三方免 key 服务（wttr.in），
  它慢或挂掉时动作会如实把 HTTP 状态报出来。
- 时区写死 UTC+8（这块板子只服务一个时区）。
- 动作空间涨到 47 个 JSON 串之后，同一个 0.26M 模型的留出集从 93.5% 掉到 87.3%——
  **容量是真的有限**。再往上加动作，要么继续扩说法重训（5~12 分钟），要么换更大的
  底座。这个取舍摆在这里，不藏。

## 十一、语音：说一句就控制（云听、端选）

### 11.1 分工，以及它是怎么定下来的

一开始想让云端一次性做完"听 + 出动作 JSON"：把音频发过去，让它直接回
`{"action":...}`。实测不行——那是个思考型模型，**800 token 全花在自言自语上**，
JSON 还没轮到输出（日志里能看到它反复念我给的提示词）。但同一段音频让它**只做
转写**，一次就对：

    我合成的 "turn on the red light" -> 转写 "Turn on the red light"

于是分工定成：**云负责听（自由说法都能听），端负责选（动作词汇表不出这块板子）**。
后者正是第十节那个自己训的 0.26M 命令模型，留出集 43/46。

### 11.2 转写模型：专用 ASR 比通用模型快 5 倍

| 用哪个模型转写 | 延迟 | 说明 |
|---|---|---|
| `mimo-v2.5`（通用 chat） | **7.7 s** | 能用，但先自言自语一段 |
| `mimo-v2.5-asr`（专用） | **1.6 s** | 要求**只发音频**：带文字提示会被拒（`ASR request must not include text parts`）|

所以 `voice_exec.c` 发的是**纯音频**请求，模型名走 `GATEWAY_ASR_MODEL`。

### 11.3 端到端实测（真机，源码对照/cloud_probe.py 造音、curl 打 /voice）

    turn on the red light     3.2s  {"transcript":"Turn on the red light.","ok":true,
                                      "result":{"action":"led","color":"red","result":"led set"}}
    switch the blue light on  3.8s  {"action":"led","color":"blue"}（留出集说法）
    could you please make the light green  11.0s（换 ASR 模型前）
                                           -> {"action":"led","color":"green"}
    scan the wifi networks    13.4s  {"action":"wifi_scan","aps":36}
    what is your status        3.2s  {"action":"status", ...}

拆开看是：上传 ~0.2 s + 转写 1.6 s + 本地命令模型 1.2 s + 动作本身（扫 WiFi 那条约
10 s 是因为扫描要 2~3 s，且它是在换 ASR 模型之前测的）。

**2026-09-19 下午补测（第二批动作 + 网络抖动）**：

    turn on the blue light               3.8 / 3.8 / 3.9 s   （连测三次，同一句）
    what is the chip temperature         4.0 s  -> 47.3 °C
    tell me a joke                       9.4 s  -> 云端给的笑话（emoji 完整）
    blink the blue light three times     6.0 s  -> {"effect":"blink","times":3}

同一天下午还撞上一次**云端 ASR 退化**：同一条音频平时 1.6 s，那次输入 43 秒才回来
（期间我打的另一条口令还被拖到 30 秒超时——两条请求共用同一个引擎互斥锁）。从那以后
转写这条路单独给了 **20 秒**上限（云端聊天那条腿仍是 45 秒）：忙的时候 20 秒如实报
失败，而不是让页面一直转。这属于"云端不是我们控制的"那一类边界，写在这里，不假装
语音腿的延迟是稳定的。

### 11.4 入口：一个手机能打开的页面

网关在 `GET /talk` 提供一个**按住说话**的页面（浏览器 `getUserMedia` 录音 →
在 JS 里降到 16 kHz 单声道、封成 WAV → `POST /voice`）。手机或电脑连同一个热点，
打开页面就能用。

**这条腿的边界写清楚**：音频输入用的是浏览器（手机/电脑的麦克风），不是板子的
麦克风——板子的采集通路单独验证过（48 kHz 立体声、零丢包，见音频证据），把这
两段接起来是下一步的事。另外**语音这条腿需要联网**（听在云端）；断网时打字的口令
仍然可用，那条路完全是本地的。

#### 11.4.1 第一版在手机上"按了没反应"——浏览器不给 http 页面麦克风

第一版页面只有一条腿：按住 → `getUserMedia` → 上传。用户实测：页面能打开，
**按钮按下去毫无反应**。

在同一台机器上用真浏览器复现（Chrome，`10.138.138.122/talk`）：

| 探测 | 结果 |
|---|---|
| `window.isSecureContext` | **false** |
| `typeof navigator.mediaDevices` | **`"undefined"`** |
| `location.protocol` | `http:` |

点按钮时控制台抓到的两条 Promise 拒绝：

```
rejection: Cannot read properties of undefined (reading 'getUserMedia')
rejection: Cannot read properties of undefined (reading 'disconnect')
```

——不是本地代码的 bug，是**浏览器只把 getUserMedia 给安全上下文**（https 或
localhost）。http 页面上 `navigator.mediaDevices` 这个对象根本不存在，于是按钮点下去
什么都不会发生（第一版也没有 try/catch，失败是静默的）。第二条拒绝是同一个坑的另一面：
没拿到 stream 时 `stop()` 仍会去 `disconnect`，所以"按一下"还会再抛一次。

#### 11.4.2 修法：同一组路由再开一个 TLS 口，页面改成三条腿

1. **TLS 口**：`espllm_http_start_secure()`（组件里用 `esp_https_server`）把**同一组路由**
   在 443 上再注册一遍，`/talk` 与 `/voice` 都在。板上 Agent 仍走明文那条腿
   （`http://10.0.0.1/v1`），两个口并存——板子的 NuttX 客户端不做 TLS，这条路不能动。
   证书是**自签**的：手机第一次打开会问一次，点"继续前往"即可（见网关 README 里
   "自签证书"一节，那里也写了它不是秘密、以及怎么重新生成）。
2. **页面按 `isSecureContext` 自己选腿**：
   - https 页：按住说话（`getUserMedia`）；
   - 传一段录音文件（`decodeAudioData` 解码后同样降到 16 kHz，手机录音机的 m4a 一般能解）；
   - 直接打字（POST `/v1/chat/completions` + `model: cmd`，这条**完全本地、断网可用**）。

   http 页面把"按住说话"置灰并**把原因写在页面上**，同时给出 https 链接——不装作能用。
3. 顺手修掉三个会咬人的地方：`stop()` 在没 stream 时的二次异常；按得太快（松开早于
   `getUserMedia` 返回）时的脏状态；`createScriptProcessor` 的输出接到扬声器会啸叫
   （接一个 gain=0）；`pointercancel` 兜住手指滑出按钮的情况。
4. **第一次按必然弹权限框**——用户不可能一边按住按钮一边去点"允许"，所以那一按什么
   都录不到。改成：拿到流之后**不释放**（松开只停录音，`pagehide` 才停轨道），第二按
   起就是即时的；文案也照着改（"如果刚才弹了授权框，先点允许，再重新按住一次"）。
   实测：第二按 400 ms 内进入"录音中"，且 `getUserMedia` 全程只被调用过一次。

#### 11.4.3 重新验证（不需要真实麦克风的部分全部验过）

| 环节 | 怎么验的 | 结果 |
|---|---|---|
| 页面 JS 语法 | `tools/check_voice_page.py`：把 C 字符串按编译器的方式解开（**并跳过 C 注释**——注释里的引号曾被当成字符串拼进 JS，这个坑把工具自己也教育了一顿），交给 `node --check` | 通过；抽出的页面与设备实际发出的 **7541 B 逐字节一致** |
| TLS 口 | 设备日志 + `curl -k https://10.138.138.122/talk` | `Server listening on port 443`，HTTP 200，页面与明文口同一份 |
| 连按两次（模拟"第一次弹权限框"之后的第二次） | 第二按后 400 ms 读页面状态 | 进入"录音中…"，且 `getUserMedia` 调用次数仍为 **1**（流被复用，不再有权限往返） |
| 打字口令（浏览器真敲） | 页面上输入 `turn on the white light` 点发送 | 1022 ms，`{"action":"led","color":"white","result":"led set"}` |
| 语音走 TLS 口 | 16 kHz 合成音 `turn on the blue light` 打 `https://…/voice` | 3.99 s，`{"transcript":"Turn on the blue light.","ok":true,…"color":"blue"}` |
| 语音走明文口（http 页面传文件那条路） | `switch the green light on` 打明文 `/voice` | 3.70 s，`{"action":"led","color":"green"}` |
| **按住说话的录音链路** | 本机没有麦克风、受控浏览器又点不过自签证书：用 `Object.defineProperty` 顶替 `isSecureContext` 与 `navigator.mediaDevices`，拿 440 Hz 振荡器当音源（48 kHz），走完 按住→编码→上传→渲染 | 页面切到"麦克风可用"分支；2.3 s 后设备回 `{"error":"the recording held no speech (silence or a tone)"}`——链路通，内容是纯音 |
| **真实麦克风** | 需要一台能点过自签证书的浏览器（手机） | **待用户实测**——这是唯一没验的一环 |

最后那一条测试还顺手暴露一个粗糙处：转写成功但内容是空的（静音/纯音）时，原来的代码
会返回 `{"error":""}`。现在改成 `the recording held no speech (silence or a tone)`，
把"听清了但没听到话"和"识别失败"分开说。


## 米家 miIO 链路（2026-09-20，软件层验证，未上板）

**背景**：要让板子控制米家设备，S3 侧新增 miIO/MIoT 局域网客户端（`main/net_miio.c`）。
手上没有真台灯，而 python-miio 0.5.12 已经移除了官方模拟器（`devtools miio-simulator`
跑不起来），所以用两层办法先把协议钉死，再留给上板半步。

### 一、报文格式与参考实现逐字段对拍（`pc_side/miio_protocol_check.py`）

裁判是 python-miio 的 `miio/protocol.py` —— 报文格式的事实标准（真机认它，社区工具也认它）。

| 核对项 | 结果 |
|---|---|
| magic / 大端 length / 16 字节头 + 32 偏移 | 一致（`2131`、length=96、总长 96） |
| **checksum 按什么算** | **按密文**：按密文重算 = 参考实现的值；按明文重算不匹配 |
| key=MD5(token)、iv=MD5(key+token) | 双向互解成功 |
| PKCS7 填充、JSON 末尾补 `0` | 一致（明文语义相同；空格差异不影响） |
| 我组出来的包 → 参考实现解析 | 成功，解出原始对象 |
| 参考实现的包 → 我的解析 | 成功，解出原始对象 |

结论：**`net_miio.c` 的报文布局与参考实现一致**。"checksum 按密文算"这条如果不做对拍，
很容易想当然写成按明文，真机就会不认——这次对拍直接把它定下来了。

### 二、被控端：自写的最小 miIO 设备模拟器（`pc_side/miio_device_sim.py`）

UDP 54321，实现 hello 握手（32 字节包回 token）与 `miIO.info` / `get_prop` / `set_power` /
`toggle` / `set_bright` / `set_ct_abx`，并维护台灯状态（power / bright / ct）。

用 **python-miio 自己的客户端**驱动它，等于让参考实现来确认它是一台"标准 miIO 设备"：

```
info()                          -> yeelink.light.color3 v1.0.0 (AA:BB:CC:DD:EE:FF)
set_power on                    -> [ok]        模拟器侧 power=on
set_bright 70                   -> [ok]        模拟器侧 bright=70
get_prop [power,bright,ct]      -> [on, 70, 4000]      ← 状态回读
toggle                          -> [ok]        模拟器侧 power=off
```

**诚实标注**：这是**模拟设备**——协议是真的，状态机是假的。换真设备的做法是把 S3 设备表
里的 IP 与 token 换掉（`tools/set_miio.py`），代码不用动；**真机迁移尚未验证**。

### 三、复现时会遇到的 Windows 坑（记下来省得再查）

Windows 会把对端 UDP 的 ICMP 端口不可达报成 `recvfrom` 上的 `WinError 10054`（连接重置），
参考客户端与自写模拟器都会因此中断。两侧都加 `sock.ioctl(socket.SIO_UDP_CONNRESET, False)`
即可。ESP32 侧不受影响（lwIP 不做这件事）。

### 四、本阶段**没有**验证的（不写成就绪）

- S3 上板执行 `model:"miio"` 分支：固件已编译通过（1,150,256 B / md5 `8b9a9379`），**未烧写**
- 手机热点下 PC 与 S3 的互相可达（必需方向是 S3 → PC:54321/udp）
- 真米家设备（需设备支持"局域网控制" + 取到 token）

## 米家链路 · 真机实测（2026-09-20）

硬件：SF32LB52-DevKit-LCD（口 A = COM5）+ ESP32-S3-DevKitC-1（USB = COM10）；
PC 与 S3 同在手机热点（S3 得 10.138.138.122，PC 10.138.138.135）；
被控端 = **PC 上自写的 miIO 设备模拟器**（官方模拟器在 python-miio 0.5.12 已移除）。

### 一、协议层（无硬件，与参考实现对拍）
见 `pc_side/miio_protocol_check.py`：magic/大端 length/16 字节头 + 32 偏移一致；
**checksum 按密文算**；AES 双向互解；我组包参考实现能解析、反之亦然。

### 二、S3 侧固件（1,150,736 B 级，含 net_miio + net_clf）
开机日志确认：`gw_miio: 设备表：2 台`（lamp@10.138.138.135:54321、ac@…:54322）、
`espllm: model … vocab 512 … weights int8 quantized`、HTTP 80 / HTTPS 443 都在。

### 三、PC → S3：确定性路由 `model:"miio"`（英文口令）
`lamp on` / `lamp brightness 70` / `lamp prop power` 三条全通，回读 `["on"]`
—— 指令真的落到了被控端，且返回已改成**中文句**（`content=台灯已打开`）。

### 四、PC → S3：本地意图分类器 `model:"cmd"`（中文口语，**全程离线**）
蒸馏语料训练出的分类器（中文留出集 15/16 = 93.8%）烧进 S3 后，真机实测 6/6：

| 输入（蒸馏出的口语说法，非模板） | S3 返回 |
|---|---|
| 打开空调 | 空调已打开 |
| 把空调关了 | 空调已关闭 |
| 帮我把灯打开 | 台灯已打开 |
| 调暗一点 | 台灯亮度已调到 30 |
| 书房的灯开开 | 台灯已打开 |
| 把台灯调暖一点 | 台灯色温已调到 2700K |

链路：一句话 → S3 本地分类器（字符 n-gram 哈希 + int8 线性）→ 局域网 miIO → 设备。
**不经过云端、不需要外网。**

### 五、板子 → PPP → S3（根链路）
`pppd /dev/ttyS0 460800 &` → `ppp0 … RUNNING`；板子发 `model:miio` 请求 15,529 B，
S3 5.0 s 回 43 B；换成中文分类器后回 15 B（`台灯已打开`），板子随即走 TTS 播报。
**但板子侧出现过两次间歇崩溃**（`ask` 之后 `sched_dumpstack`，控制台随后无输出），
断言原文尚未捕获 —— 记为**未解决**，见下。

### 六、如实标注（本次没有证据的部分）
1. **没有控制过任何真实小米设备。** 被控端全程是模拟器；协议是真的、设备是假的。
2. 我们的 miIO 客户端目前只实现**老式 miIO 方法**（`set_power`/`toggle`/`set_bright`/
   `set_ct_abx`/`get_prop`/`miIO.info`，即 Yeelight 系与部分米家设备那套）；
   **较新的 MIoT-spec 设备（siid/piid）尚未实现**，需要按型号取 spec 再做映射。
3. 真机落地的前置条件（缺一不可）：设备支持"局域网控制"、取到 IP+token、
   S3 与设备同网。任一条不满足，只能说"协议通了"，不能说"控住了那台设备"。
4. 未解决：板子侧 `ask` 的间歇崩溃；S3 侧 `etif_lwip-ppp: pppos_input_tcpip failed with -1`
   刷屏（疑与"在线 TTS 大响应 ~20 KB 截断"同源）。

## 米家链路升级：改用**官方模拟器**当被控端（2026-09-20 晚）

上一节自写的模拟器只是权宜之计；官方模拟器其实还在，只是我上次只查了 0.5.12 就下了
"已移除"的错误结论。**python-miio 0.6.0.dev0 里有官方模拟器**：

    miio/devtools/simulators/miiosimulator.py + miotsimulator.py
    miiocli devtools miio-simulator --file <YAML>      # 另有 miot-simulator

设备描述是 YAML（字段照官方 pydantic 模型）：
`name / models[{model,name}] / type / properties[{name,type,value,setter,min,max}] / methods[{name,result}]`
本仓库自带一份：`pc_side/yeelight_lamp.yaml`（power/bright/ct 三个**带 setter 的属性**）。
官方实现把令牌固定为全零（`miio/devtools/simulators/common.py`: `"token": 32 * "0"`），
因此 S3 设备表里那一行也用全零。

### 官方侧日志（未改一行官方代码）

```
INFO  MiioModel(model=yeelink.light.color3, name=Desk

## 米家链路升级：改用**官方模拟器**当被控端（2026-09-20 晚）

上一节自写的模拟器只是权宜之计；官方模拟器其实还在，只是我上次只查了 0.5.12 就下了
"已移除"的错误结论。**python-miio 0.6.0.dev0 里有官方模拟器**：

    miio/devtools/simulators/miiosimulator.py + miotsimulator.py
    miiocli devtools miio-simulator --file <YAML>      # 另有 miot-simulator

设备描述是 YAML（字段照官方 pydantic 模型）：
`name / models[{model,name}] / type / properties[{name,type,value,setter,min,max}] / methods[{name,result}]`
本仓库自带一份：`pc_side/yeelight_lamp.yaml`（power/bright/ct 三个**带 setter 的属性**）。
官方实现把令牌固定为全零（`miio/devtools/simulators/common.py`: `"token": 32 * "0"`），
因此 S3 设备表里那一行也用全零。

### 官方侧日志（未改一行官方代码）

```
INFO  MiioModel(model='yeelink.light.color3', name='Desk Lamp')
INFO  Miio push server started with address=0.0.0.0   server_id=706233514
INFO  Got setter call with {'id': 25173, 'method': 'set_power',  'params': ['on', 'smooth', 500]}
INFO  Got setter call with {'id': 21546, 'method': 'set_bright', 'params': [70, 'smooth', 500]}
```

### 从 S3 打它的结果（`model:"miio"`，全程局域网、不经云端）

| 指令 | S3 返回 |
|---|---|
| lamp on | 台灯已打开 |
| lamp brightness 70 | 台灯亮度已调到 70 |
| lamp prop power | 台灯现在是开着的（**状态回读**） |

**这一步把验证强度提上来了**：被控端是**官方实现**，不再是"我写的模拟器像不像设备"，
而是"官方代码收下并执行了我客户端的指令"。裁判从我自己换成了 python-miio。

### 仍未验证 / 未解决（口径不变）
1. **真机零验证**：没有控制过任何真实小米设备（本节点亮的是官方模拟器，不是真灯）。
2. **MIoT-spec（siid/piid）未实现**：目前只覆盖老式 miIO 方法（set_power/toggle/
   set_bright/set_ct_abx/get_prop/miIO.info）。官方另有 `miot-simulator`，可用于
   把这部分补齐。
3. 板子侧 `ask` 后的间歇崩溃、S3 侧 `pppos_input_tcpip failed` 刷屏 —— 均未解决。
