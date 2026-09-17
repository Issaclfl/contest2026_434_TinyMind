# TinyMind-Audio —— 让一块"哑巴"板子开口说话

> 2026 首届 openvela AI 硬件开发者大赛 ｜ 队伍 **TinyMind**（编号 434）
> 选题方向：**新硬件平台适配**（叠加 AI 硬件产品创新）

---

## 一、作品主张

**这块板子的官方定位是"蓝牙音频终端"，出厂却没有音频驱动，是个哑巴。
我们从零补上它的声音，再接上 openvela 官方的 AI Agent 引擎，
让它第一次能用「你好，openvela」对话。**

两半指向同一个空白：

- 官方把 SF32LB52-DevKit-LCD 定位为音频终端（板载麦克风、喇叭、功放使能线全都焊在
  PCB 上），**却没给音频驱动**——芯片的 AUDCODEC / AUDPRC 在 openvela 树里没有一行
  代码知道怎么指挥。
- 官方端侧 AI Agent 引擎（`packages/ai_agent`，即官网所称 **openvelaClaw**）把语音
  列为交互通道之一，**却没适配这块板子**——官方 defconfig 只覆盖 QEMU、ESP32-S3、
  Gemini-S1 三块。

我们的工作就是填掉这两处空白，并把两者接起来：**先让板子有耳朵和嘴，再让它有脑子。**

---

## 二、三层交付，每层都可独立验证

### 第 1 层：音频底座（已完成，真机 8/8 验证）

从零为 SF32LB52 写 NuttX 标准音频 lower-half 驱动：

- 适配后系统出现标准设备 `/dev/audio0`，应用可用 `open/ioctl/read` 通用接口录音放音
- 完整覆盖 **AUDCODEC（模拟↔数字）+ AUDPRC（数字路由）** 双器件的上电、时钟、路由配置
- 逐行对齐芯片厂参考驱动，实现**无爆音的功放上电/下电时序**

这不是"调用现成 SDK 示例"，而是从零把一颗芯片的音频子系统接进操作系统框架——需要同时
理解 NuttX 驱动模型（函数表、上下半身分离、缓冲队列）与芯片模拟时序（Σ-Δ ADC 时钟树、
功放防爆音）。

### 第 2 层：AI Agent 移植（已完成，真机跑起来了）

把官方 openvelaClaw 引擎移植到 SF32LB52。官方 `packages/ai_agent/defconfigs/` 原本只支持：

| 已支持 | 架构 |
|---|---|
| goldfish-arm64-v8a-ap | arm64（QEMU 模拟器） |
| esp32s3-eye | xtensa 32 位（ESP32-S3） |
| gemini-s1 | armv7-a（全志 R528） |

**SF32LB52 是第四块，也是 armv8-m 上的第一块。**

真机实测（非模拟、非估算）：Agent 启动完成，**36 个工具注册**、Skills 系统装载、
配置写入成功。

### 第 3 层：IP 承载（软件完成并经真机验证，只差一根线）

这块板子**没有任何网卡**：vendor 树里没有 WiFi 驱动，芯片不带以太网 MAC，片内 USB 是
CDC ACM 串口（也就是控制台）。开箱状态是 `CONFIG_NET` 关闭、整板只有一条串口调试线。
没有 IP，Agent 就够不到 LLM 端点。

我们补上了这条缺失的传输层：**UART2 上跑 PPP**，PC 侧做网关。SLIP 走不通——Linux 5.14
起移除了内核 SLIP 支持——所以用 PPP。

### 对照「基于 openvela 开发的判定标准」

大赛规定：项目须使用 **openvela 开源项目（NuttX 内核仓库除外）** 提供的系统能力，且至少
落地**图形、AI、多媒体**三项核心能力之一。本作品逐项对照如下——

| 用到的能力 | 所在仓库 | 是否 openvela | 在本作品中承担什么 |
|---|---|---|---|
| 板级适配（bringup / defconfig / CMake / 链接脚本） | `vendor/sifli` | ✅ vendor 仓 | 第 1 层的载体，也就是「新硬件平台适配」这条赛道的定义本身 |
| 构建系统（`build/envsetup.sh`、cmake 模块、`lunch`/`m` 流程） | `build/` | ✅ build 仓 | 三层交付全部经它集成与验证 |
| **AI Agent 框架 openvelaClaw** | `packages/ai_agent` | ✅ packages 仓 | **第 2 层：AI 能力落地** |
| QuickApp / 应用框架脚手架 | `app/`、`quickapp/` | ✅ 组委会下发 | 应用侧扩展入口 |
| LVGL 图形框架 + `lvgldemo` | `apps/graphics/lvgl` | ✅ apps 仓 | **图形能力**；`lvgldemo`、`fb` 均已在固件中确认为内置命令 |
| NuttX 音频 lower-half 接口 | `nuttx/audio` | ⚠️ NuttX 仓（判定标准中排除） | 第 1 层的**实现接口**，不是成果本身 |

关于最后一行需要说明清楚：音频驱动所依托的 `nuttx/audio` 接口确实属于被排除的 NuttX 内核仓，
但**本作品的成果不是「调用了这个接口」**——是这块芯片的音频子系统此前在 openvela 上完全
不存在，我们把它从零补了出来，并经 openvela 的 vendor 板级层与 build 构建系统交付。在此之上
落地的 **AI（openvelaClaw，第 2 层）** 与 **图形（LVGL，板级已就绪、`lvgldemo` 可直接跑）**
两项能力，加上可现场演示的音频采集，共同构成「至少落地三项核心能力之一」的实证。

---

## 三、真机验证结果

### 音频（第 1 层）

| 证据 | 结果 |
|------|------|
| `ls /dev` | 出现 **`audio0`** |
| `nxrecorder` 串口日志 | `capture config: 48000Hz 2ch 16bit` → `capture started (staging 16384 B, circular)` → `stop: overruns=0` |
| 录音文件 | **262,144 字节** PCM |
| 音频内容 | 峰值 16360 / RMS 356.4，12.7 万样本中 12.6 万非零，波形平滑连续 → ADC 在真实转换模拟输入 |
| 重启后复测 | 8/8 项全部通过（含暂停/恢复、破坏性停止后的重建） |

### AI Agent（第 2 层）

| 项目 | 结果 |
|---|---|
| 构建产物 | `nuttx.bin` **2,229,560 字节**，md5 `77bff8fd5312bfa6bb8e7981665d8d9d` |
| 资源占用 | flash 13.03%（16 MB）／ sram 29.24%（512 KB）／ PSRAM 已在堆内 |
| 运行期内存 | `free` 报 Umem 总量 **8.7 MB**（512 KB SRAM + 8 MB PSRAM，`CONFIG_MM_REGIONS=2`） |
| 板子实跑 | `ai_agent` 启动完成，**36 个工具**注册、Skills 装载、`set_llm` 写入成功 |
| 注册为 NSH 内置命令 | `{ "ai_agent", 100, 32768, ai_agent_main }`，栈 32 KB |
| **音频回归**（新增网络配置是否碰坏音频） | Agent 固件上跑 `nxrecorder` 两轮，落盘 933,888 / 745,472 字节，**两轮 `overruns=0`** |
| **堆占用稳定性**（是否泄漏） | 连跑三次 Agent 启停，堆占用均为 2,098,576 字节，**完全一致 → 无泄漏**；总堆 8.7 MB 余 6.6 MB |

### IP 承载（第 3 层）

PC 侧每一环都拿真硬件跑过：

| 环节 | 怎么验的 | 结果 |
|---|---|---|
| WSL ↔ Windows 宿主 TCP | 两个方向对打 | 通 |
| Windows 串口桥：COM ↔ TCP 双向 | 桥接板子控制台，从 WSL 发 `uname -a` | 板子执行并把结果送回 |
| socat：TCP ↔ PTY | 与桥串起来跑上面那条 | 通 |
| pppd 协商 + 分配 IP + 数据面 | WSL 内两个 PTY 对接跑两个 pppd | LCP/IPCP 成功，ppp0/ppp1 起来，**跨链路 ping 3/3 零丢包** |
| 板子 `/dev/ttyS0` 可打开、可改速率 | `pppd /dev/ttyS0 460800` | 通过 |
| `pppd` 内置命令 + 直连模式 | `pppd -h` 打印用法 | 通过（补丁在真机生效） |
| **UART2 ↔ USB-TTL ↔ 桥 ↔ pppd** | 需要三根杜邦线 | 待接线 |

### 诚实说明

- **放音出声尚未实测**：驱动侧放音链路已实现并通过编译与符号验证，但因缺 4Ω 喇叭未做
  实机出声测试。技术报告 3.5.4 与 3.7.3 有完整记录。
- **语音通道（第 3 层之上的"对话"）未完成，且已查清为什么**：Agent 的默认语音后端走
  Vela 媒体框架（`media_recorder`），而该框架建在 FFmpeg + PFW 插件之上
  （`frameworks/multimedia/media/`），`CONFIG_MEDIA` 依赖 `LIB_FFMPEG`——在 armv8-m 上
  把这套媒体守护进程移植过来是另一个独立工程。所以本作品**只承诺文本对话**，语音通道
  如实标记为后续工作。
- 本仓不含未经实测的性能数据。

---

## 四、顺带修掉的一个挡住所有参赛队的缺陷

`apps/CMakeLists.txt:78` 是 `if(EXISTS packages/CMakeLists.txt)`，而基础 manifest
`openvela.xml:206` 的 `<project path="packages" name="packages"/>` 在当前工程里
**没有同步下来**，于是该文件缺失，**整棵 `packages` 树从不参与构建**——组委会自己发的
`app/hello_app` 骨架（映射到 `packages/demos/`）同样编不进去。

从 Gitee 的 `open-vela/packages`（`dev-ai-contest-2026` 分支）取回官方文件即可。它只有
两行实质内容，且同时解决 CMake 与 Kconfig 两道闸门（`nuttx_generate_kconfig` 说明该树的
Kconfig 是代码生成的）：

```cmake
nuttx_add_subdirectory()
nuttx_generate_kconfig(MENUDESC "Packages")
```

---

## 五、仓库结构

```text
contest2026_434_TinyMind/
├── board/sf32lb52_audio/          ⭐ 第 1 层：音频驱动适配
│   ├── src/bsp_audio.c            # NuttX audio lower-half 适配层
│   ├── src/bsp_audio_hw.c         # 硬件上电/时钟/路由/防爆音时序
│   ├── include/                   # 对外头文件
│   ├── board_files/               # 需覆盖到生产树的文件
│   ├── patches/                   # 上述改动的标准 diff
│   ├── apply.sh                   # 一键落盘脚本
│   ├── ai_agent_port/             ⭐ 第 2 层：AI Agent 移植
│   │   ├── configs/ai_agent/defconfig
│   │   ├── packages-CMakeLists.txt
│   │   └── README.md              # 移植说明（含 packages 树缺陷的来龙去脉）
│   └── README.md                  # 音频适配详细说明
├── board/sf32lb52_net/            ⭐ 第 3 层：IP 承载（PPP over UART2）
│   ├── patches/                   # 板侧 3 处改动
│   ├── pc_side/                   # 串口桥 + socat + pppd + NAT 脚本
│   ├── apply.sh
│   └── README.md                  # 接线表、跑通顺序、上板踩到的两个陷阱
├── .claude/skills/                # 沉淀的 AI 开发 Skill
│   └── nuttx-audio-lowerhalf-porting/
├── logs/                          # AI Coding 日志
├── docs/                          # 作品介绍文档、上板验证清单、技术报告
└── contest2026_434_TinyMind.xml   # manifest（已增补三层交付的 linkfile）
```

---

## 六、快速开始

### 音频底座（第 1 层）

```bash
# 1) 拉取工程
repo init -u https://github.com/open-vela/contest2026_434_TinyMind \
  -b dev-ai-contest-2026 -m contest2026_434_TinyMind.xml
repo sync -c -j8

# 2) 落盘生产树改动（驱动源文件由 manifest 自动就位）
bash contest2026_434_TinyMind/board/sf32lb52_audio/apply.sh

# 3) 编译
source build/envsetup.sh
lunch vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/nsh
m -j8
```

验证：串口（1000000 8N1）进 NSH 后执行 `ls /dev`，应看到 **`audio0`**。

### AI Agent（第 2 层）

```bash
# 1) 补回 packages 构建闸门（若工程里还没有）
cp contest2026_434_TinyMind/board/sf32lb52_audio/ai_agent_port/packages-CMakeLists.txt \
   packages/CMakeLists.txt

# 2) 落板级配置
cp contest2026_434_TinyMind/board/sf32lb52_audio/ai_agent_port/configs/ai_agent/defconfig \
   vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/ai_agent/defconfig

# 3) 打工具链兼容性补丁
git -C packages/ai_agent apply \
   contest2026_434_TinyMind/board/sf32lb52_audio/patches/0005-ai_agent-fix_sf32lb52.patch

# 4) 构建 + 烧录
source build/envsetup.sh
lunch vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/ai_agent
m -j8
sftool -c SF32LB52 -p COM5 -b 1000000 \
       --before default_reset --after soft_reset \
       write_flash <nuttx.bin 绝对路径>@0x12010000
```

验证：NSH 里执行 `ai_agent`，看到 `vela>` 提示符与 36 个工具注册日志。

### IP 承载（第 3 层）

先把 USB-TTL 接到板子 UART2（**PA20=RX / PA27=TX / GND**），然后：

```bash
# 1) 板子：后台起 pppd（前台跑会占死控制台）
nsh> pppd /dev/ttyS0 460800 &

# 2) Windows：把 USB-TTL 的 COM 口挂到 TCP 上
python serial_tcp_bridge.py COM7 460800

# 3) WSL：起 ppp0 + NAT + MSS 夹取（宿主地址自动探测）
wsl.exe -d Ubuntu-24.04 -u root -e bash -lc \
  'bash <repo>/board/sf32lb52_net/pc_side/ppp_up.sh'

# 4) 板子上验证
nsh> ifconfig          # ppp0 应拿到 10.0.0.2
nsh> ping 223.5.5.5
```

之后在 `vela>` 里 `set_llm <host> <model> <key>` 写入端点即可对话。

---

## 七、技术要点

1. **上下半身分离**：`sf32lb52_audio_s` 首成员为 `struct audio_lowerhalf_s`，上层仅持有
   "基类"指针，各回调强转回完整结构体 —— NuttX 标准 lower-half 模式。
2. **双物理器件**：AUDCODEC 负责模拟↔数字转换（PGA/ADC/DAC），AUDPRC 负责数字路由/
   混音/EQ，分别配置。
3. **时钟表照抄厂商**：16K/48K 两族由 48MHz XTAL 分频，Σ-Δ 的 OSR 与 chop 配比直接沿用
   SiFli-SDK 验证过的表；表外采样率明确返回 `-EINVAL`，不做无法兑现的能力宣称。
4. **防爆音时序**：放音严格 `静音 → 数字使能 → 模拟分步上电 → 10ms → 开 PA → 100ms →
   解除静音`，停止严格逆序；录音为 `模拟先上 → MICBIAS 稳定 → 数字使能 → DMA 最后`。
5. **破坏性停止 / 非对称重建**：`hw_stop()` 会清 AUDPRC 通道配置并软复位整个块（连分频都
   丢），而 `hw_start()` 只做使能——所以 resume 必须完整重新配置而不能只重新使能。这是一
   个"编译通过、上板才炸"的静默缺陷，技术报告里有完整记录。
6. **工程化交付**：所有生产树改动都提供 `apply.sh`（文件覆盖式、幂等）+ `patches/`（标准
   diff）双形式，驱动源文件经 manifest `<linkfile>` 自动就位，**生产仓新增代码零侵入**。

---

## 八、AI-Native 开发

- 全程使用 AI 编程工具完成驱动适配、Agent 移植、网络承载、构建问题排查与文档撰写
- 沉淀 Skill：[`.claude/skills/nuttx-audio-lowerhalf-porting/`](.claude/skills/nuttx-audio-lowerhalf-porting/SKILL.md)
  —— 把"新芯片音频 lower-half 移植"的方法论与踩坑经验固化为可复用能力
- AI Coding 日志见 `logs/`

---

## 九、团队分工

| 成员 | 职责 |
|------|------|
| Issaclfl | 驱动实现、Agent 移植、网络承载、构建集成、技术文档 |
| 队友 | 演示视频、实物照片、海报 |

---

## 十、许可

Apache License 2.0。详见各源文件头部声明。
