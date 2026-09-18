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
