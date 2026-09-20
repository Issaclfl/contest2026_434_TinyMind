# AI Agent 端到端实测证据（2026-09-18）

## 被测配置

- 固件：ai_agent，`nuttx.bin` 2,229,560 字节，md5 `77bff8fd5312bfa6bb8e7981665d8d9d`
  （**本文记录的是那一次实测当时的固件**，故 md5 不动。此后固件在音频侧补了
  `nxplayer` 与放音/采集通道配置修复，**当前随包固件为 2,249,088 字节 / md5
  `4fa0eade7844f22a55f561bc7723aa91`**，Agent 部分的功能与本文一致。
  音频侧的新证据见 `docs/audio-evidence/playback-loop-48k-stereo.log` 与
  `board/sf32lb52_audio/README.md` §7.8。）
- 通路：**芯片原生 USB（CDC ACM）** —— 板子 `/dev/ttyACM0` ↔ PC `COM9`（VID:PID `38F4:A4A7`）
- **全程只用一根 USB 线**，未使用 USB-TTL，未接任何排针
- PC 侧链路：Windows 串口桥 → TCP → WSL socat → pppd → ppp0 → NAT → 外网
- LLM：`https://token-plan-cn.xiaomimimo.com/v1/chat/completions`，模型 `mimo-v2.5-pro`

## 一、链路建立

```
步骤 3：在 WSL 里起 ppp0 + NAT + MSS 夹取 ━━━━━━━━━━━━━━━━
[ppp_up] 出口接口 eth0 (MTU 1280)，MSS 夹到 1240
[ppp_up] PTY 就绪：/tmp/ttyPPP -> 172.18.176.1:5555
[ppp_up] 转发与 NAT 就绪
[ppp_up] 链路已建立：
         5: ppp0: <POINTOPOINT,MULTICAST,NOARP,UP,LOWER_UP> mtu 1500 qdisc fq_codel state UNKNOWN group default qlen 3
             inet 10.0.0.1 peer 10.0.0.2/32 scope global ppp0
                valid_lft forever preferred_lft forever
[ppp_up] 板子侧地址 10.0.0.2，日志 /tmp/ppp.log

━━━ 步骤 4：从板子控制台验证 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
ifconfig
ppp0	Link encap:TUN at RUNNING mtu 1500
	inet addr:10.0.0.2 DRaddr:10.0.0.1 Mask:0.0.0.0

nsh> [K

ping 223.5.5.5
PING 223.5.5.5 56 bytes of data
56 bytes from 223.5.5.5: icmp_seq=0 time=130.0 ms

→ 之后在板子上跑 ai_agent，用 set_llm 写入 LLM 端点即可对话：
     nsh> ai_agent
     vela> set_llm https://<host>/v1 <model> <api_key>
     vela> ask 你好

━━━ 完成 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
拆链路：wsl.exe -d Ubuntu-24.04 -u root -e bash -lc 'bash /mnt/c/Users/Lawson/Desktop/大三上/openvela AI硬件开发者大赛/源码对照/ppp_down.sh'
（本脚本退出不会关掉桥；要停它请结束对应的 python 进程）
```

## 二、Agent 启动即认到网络

关键对比。**补上 PPP 承载之前**，Agent 启动时报的是：

```
[netmgr] Timed out waiting for network
[agent] Network timeout — net services not started.
```

**补上之后**：

```
[netmgr] Found iface ppp0 addr 10.0.0.2
[netmgr] Network connected: 10.0.0.2
[agent] Network connected: 10.0.0.2
[agent] Agent loop started, free heap: 8186248
[ws] WebSocket server started on port 28789
[agent] All network services started!
```

## 三、真实对话（经上述链路，TLS 1.2）

```
=== ask===
[agent] Processing message from cli:console


[skills] Skills summary: 814 bytes


[context] System prompt built: 3289 bytes


[llm] LLM config updated atomically: token-plan-cn.xiaomimimo.com/v1/chat/completions (model: mimo-v2.5-pro)


[trace:7c54306d00000000] BEGIN chat=console chan=cli


[llm] OpenAI API with tools (model: mimo-v2.5-pro, 14905 bytes)


[vela_tls] Handshake start: Host=token-plan-cn.xiaomimimo.com, UNIX=2085892205


[agent] Dispatching response → cli:console



[Agent]: 让我查一下...
vela> [agent] [Agent]: 让我查一下...


Sent to agent: 你好，用一句话介绍一下你自己
[agent] ask: 你好，用一句话介绍一下你自己


vela> [vela_tls] Handshake OK: TLSv1.2 / TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256


[llm] Response: 157 bytes text, 0 tool calls, finish=end_turn


[agent] LLM resp: text=157, tool_use=0, calls=0


[trace:7c54306d00000000] iter=0 tool=(none) latency=17406ms llm=ok backend=0


[trace:7c54306d00000000] END status=ok iters=1 tools=0 llm_ms=17406 elapsed=18s backend=0


[agent] Dispatching response → cli:console



[Agent]: 你好！我是运行在 Vela 嵌入式设备上的 AI 助手，能帮你搜索信息、查天气、设提醒、控制音乐等，有什么需要尽管说～
vela> [agent] [Agent]: 你好！我是运行在 Vela 嵌入式设备上的 AI 助手，能帮你搜索信息、查天气、设提醒、控制音乐等，有什么需要尽管说～
```

```
=== ask现在几点了？ ===
[agent] Processing message from cli:console


[tools] Executing tool: get_current_time


[tool_time] Fetching current time...


[tool_time] Time (local clock): 2036-02-06 14:30:33 CST (UTC+8), UNIX epoch: 2085892233


[agent] NL fast path: get_current_time (fast=1 llm=1)


[agent] Dispatching response → cli:console



[Agent]: 2036-02-06 14:30:33 CST (UTC+8), UNIX epoch: 2085892233
vela> [agent] [Agent]: 2036-02-06 14:30:33 CST (UTC+8), UNIX epoch: 2085892233


Sent to agent: 现在几点了？
[agent] ask: 现在几点了？


vela>
```


## 四、这组证据说明什么

1. **板子有了真实 IP**：`ppp0 inet addr:10.0.0.2`，且 `ping 223.5.5.5` 有回应（130 ms）
2. **TCP 与 TLS 跑得通**：`[vela_tls] Handshake OK: TLSv1.2 / TLS-ECDHE-RSA-WITH-CHACHA20-POLY1305-SHA256`
3. **Agent 的完整回合跑通**：`[trace] END status=ok iters=1 llm_ms=17406 backend=0`
4. **工具系统可用**：`[tools] Executing tool: get_current_time`
5. **不依赖任何额外硬件**：全程只有一根 USB 线

## 五、已知限制（如实记录）

- **板子每次复位或重新烧录后，PC 侧的 COM9 会打不开，必须把那条线物理拔插一次。**
  可稳定复现。加了 USB 探针后看到，Windows 在反复对端点 `0x83`（CDC 的中断通知
  端点）发 `CLEAR_FEATURE(HALT)`，即该端点一直处于 stall 状态——很可能是 `usbser`
  建通知管道失败导致端口不可用，拔插后能建立成功。已定位到这一层，未继续修。
- 单次 LLM 往返约 17 秒：用的是推理模型，且经过 PPP 链路。
- 每次重启后 `/data` 是 tmpfs，`set_llm` 写进去的配置需要重设。

## 六、复现步骤

```bash
# 板子（控制台 COM5，1000000 8N1，口 A 的线保持插着）
nsh> pppd /dev/ttyACM0 115200 &

# PC（口 B 的线插着，若 COMx 打不开就先拔插一次）
python ppp_e2e.py --mode usb --port COM9

# 板子上
nsh> ai_agent
vela> set_llm https://token-plan-cn.xiaomimimo.com/v1 mimo-v2.5-pro <api_key>
vela> ask 你好，用一句话介绍一下你自己
```

## 七、2026-09-20 补充：在线 TTS —— 云端契约在 PC 侧验证、板上链路的实测边界

**背景**：板上调 MiMo 的 TTS（`mimo-v2.5-tts`，走 `/v1/chat/completions`，
见 `packages/ai_agent/src/voice/mimo_tts.c`）在真机上反复拿不到音频，
板上日志是：

```
[mimo_tts] attempt 1: HTTP 200, 20196 bytes, no audio
```

量级对不上，所以做了一次**对拍**：PC 用与板上 `build_body()` **逐字节相同**的请求
打同一个端点（工具已入库：`board/sf32lb52_audio/tools/tts_probe_pc.py`，
key 只从被 gitignore 的 `sdkconfig` 读、不打印，响应只落系统临时目录）。

| 指标 | PC 侧实测 | 板上实测 |
|---|---|---|
| HTTP 状态 | 200 | 200 |
| 响应体大小 | **113,187 字节** | **20,196 字节** |
| JSON 是否完整 | 是（`json.loads` 通过，以 `}` 收尾） | 否（在 ~20 KB 处断） |
| `choices[0].message.audio.data` | 112,700 个 base64 字符 | 无 |
| 解出的音频 | 84,524 字节 WAV：**24 kHz / 单声道 / 16-bit，约 1.8 s** | — |

**结论（如实记录）**

1. **云端 TTS 契约是正确的**，返回的是真实可播音频（PC 侧已解出 WAV 并核对头字段）。
2. **板子这条链路在 ~20 KB 处停住**。排除两个嫌疑：不是我们的接收缓冲
   （`MIMO_TTS_RESP_CAP` 是 768 KB，`mimo_tts.c:64`），也不是 API——
   同一个请求在 PC 侧 113 KB 完整返回。**剩下的是链路本身**。
3. 一句话 1.8 秒就要 113 KB ⇒ 想用"把句子切短"塞进 20 KB 以内，得切到 0.3 秒一句，
   不可用。因此线上播报记为：**云端契约已验证、板上链路未打通**。
4. 语音播报这条路，演示走**本地离线片段**（`/etc/clips/*.pcm`，romfs 随固件，
   零网络依赖），该路径已真机验证（`overruns=0`）。
5. 期间试过一次把板子 PPP 的 `CONFIG_NET_TUN_PKTSIZE` 从 1500 调到 1280（怀疑大包被
   黑洞），但那次测试时 `ppp0` 处于 DOWN、回包为 0 字节，**结果不可解释**；
   该改动已回滚，仓库与文档都保持 1500。

**复现**

```bash
# PC（Windows），默认从网关 sdkconfig 读 key
python board/sf32lb52_audio/tools/tts_probe_pc.py "你好，我是 TinyMind"
```

---

## 八、离线播报接入米家答复（2026-09-20）

### 8.1 为什么要改

演示要的是"和官方模拟器互通时，板子上屏 **并且** 出声"。原实现里播报层只有两条路：

| 答复形态 | 走哪条 | 断网时 |
|---|---|---|
| 动作 JSON（`{"action":"led",...}`） | `action_announce()` → `/etc/clips` 片段 | 有声音 |
| 其它文本 | `voice_say_text()` → 云端 TTS（mimo） | **没声音**，只剩屏幕 |

网关判出来的米家答复是**一句话**（`台灯已打开`），既不是动作 JSON，断网时也够不到云 TTS，
所以演练断网演示时板子会变成"哑巴"。

### 8.2 改法

`voice_say.c` 的 `voice_say_text()` 开头加一张"句子 → 片段"表，命中就走本地片段：

```c
const char* offline = offline_clip_for(text);

if (offline != NULL) {
    syslog(LOG_INFO, "[%s] offline phrase -> clip '%s'\n", TAG, offline);
    return voice_say_clip(offline);
}
```

`offline_clip_for()` 按**子串**匹配、具体行在前，覆盖网关可能输出的全部句子：

| 匹配 | 片段 |
|---|---|
| 台灯已打开 / 台灯现在是开着的 | `lamp_on` |
| 台灯已关闭 / 台灯现在是关着的 | `lamp_off` |
| 空调已打开 / 空调现在是开着的 | `ac_on` |
| 空调已关闭 / 空调现在是关着的 | `ac_off` |
| 亮度已调到（数字因句而异） | `bright_set` |
| 色温已调到（数字因句而异） | `ct_set` |
| 没有回应 | `no_reply` |
| 没有找到这台设备 | `no_device` |
| 已切换 / 状态正常 / 状态已读回 / 已执行 | `ok` |

补丁：`patches/0008-ai_agent-offline-announce.patch`。片段清单与生成脚本：
`clips/`（16 条 .pcm）、`tools/make_voice_clips.py`（云端 TTS 预合成，**合成时联网、播放时离线**）。

### 8.3 真机日志（原文）

2026-09-20 14:5x，板子 `nsh>` → `pppd /dev/ttyS0 460800 &` → `ai_agent` →
`set_llm https://10.0.0.1/v1/chat/completions cmd local` → 连问四句：

```
>>> ask 打开台灯
[Agent]: 台灯已打开
[say] speaking 15 bytes: "台灯已打开"
[say] offline phrase -> clip 'lamp_on'
sf32lb52_audio_hw_config_playback: playback config: 16000Hz 1ch 16bit
[audio_pb] opened /dev/audio0 (16000Hz 1ch 16bit, 2 x 8192 B)
sf32lb52_stop: stop: overruns=0
[audio_pb] closing (30720 bytes written, complete=1)
[say] clip lamp_on: 30720 bytes (1.0 s)

>>> ask 关闭台灯
[Agent]: 台灯已关闭
[say] offline phrase -> clip 'lamp_off'
sf32lb52_stop: stop: overruns=0
[audio_pb] closing (35840 bytes written, complete=1)
[say] clip lamp_off: 35840 bytes (1.1 s)

>>> ask 打开空调
[Agent]: 空调已打开
[say] offline phrase -> clip 'ac_on'
sf32lb52_stop: stop: overruns=0
[say] clip ac_on: 35840 bytes (1.1 s)

>>> ask 台灯调亮一点
[Agent]: 台灯亮度已调到 80
[say] offline phrase -> clip 'bright_set'
sf32lb52_stop: stop: overruns=0
[say] clip bright_set: 56320 bytes (1.8 s)
```

四句都：`[Agent]:` 上屏 → `offline phrase` 命中 → 片段播完 `overruns=0`。

同一段日志里的 `[mimo_tts] attempt 1/2/3: HTTP 401` 属于"思考中"那句
（`让我查一下… / 稍等，处理中… / 正在分析…`）在试云端 TTS；本次板子上的 key 是占位值 `local`，
所以 401 是预期的。**答复本身没走云 TTS**，这也是断网可用的前提。

### 8.4 如实标注

- 数字类答复（亮度/色温）播报的是**通用句**（"台灯亮度已调好"），屏幕上是精确句
  （"台灯亮度已调到 80"）。为每个取值预合成不值得占的 flash。
- "思考中"的提示语离线时**不发声**（它只在云 TTS 里有），演示时表现为约 2 秒静默后直接播报结果。
- 本次四句连问**没有复现** `ask` 后的间歇崩溃（`sched_dumpstack`）；该问题仍未定位，见
  `米家生态控制方案.md` §11.5。
