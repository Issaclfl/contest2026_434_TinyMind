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
