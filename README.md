# TinyMind —— SF32LB52 板载音频驱动适配

> 2026 首届 openvela AI 硬件开发者大赛 ｜ 队伍 **TinyMind**（编号 434）
> 选题方向：**新硬件平台适配**

---

## 一、作品简介

**一句话**：给一块"哑巴"开发板装上耳朵和嘴。

SF32LB52-DevKit-LCD 出厂时是半成品：屏幕和触摸的驱动齐全，**但板载麦克风与喇叭没有任何系统级驱动**——
硬件焊在 PCB 上，openvela/NuttX 里却没有一行代码知道怎么指挥它们。

本项目为这块板补上了 NuttX **标准音频 lower-half 驱动**：

- 适配后系统出现标准设备 `/dev/audio0`，应用可用 `open/ioctl/read` 等通用接口录音与放音
- 完整覆盖 **AUDCODEC（模拟↔数字）+ AUDPRC（数字路由）** 双器件的上电、时钟、路由配置
- 逐行对齐芯片厂参考驱动，实现**无爆音的功放上电/下电时序**

**亮点**：这不是"调用现成 SDK 示例"，而是从零把一颗芯片的音频子系统接进操作系统框架——
需要同时理解 NuttX 驱动模型（函数表、上下半身分离、缓冲队列）与芯片模拟时序（Σ-Δ ADC 时钟树、功放防爆音）。

---

## 二、选题方向与赛道契合度

| 赛道要求 | 本作品 |
|----------|--------|
| 新硬件平台适配 | 为 SF32LB52 补齐 openvela 缺失的音频子系统 |
| 落地图形 / AI / 多媒体核心能力之一 | **多媒体**（音频采集与回放） |
| 使用 openvela 系统能力 | NuttX 音频框架、VFS 设备模型、board bringup |
| 原创、Apache 2.0 | 原创实现，Apache 2.0 |

---

## 三、仓库结构

```text
contest2026_434_TinyMind/
├── board/sf32lb52_audio/        ⭐ 作品核心：音频驱动适配
│   ├── src/bsp_audio.c          # NuttX audio lower-half 适配层
│   ├── src/bsp_audio_hw.c       # 硬件上电/时钟/路由/防爆音时序
│   ├── include/                 # 对外头文件
│   ├── board_files/             # 需覆盖到生产树的 4 个文件
│   ├── patches/                 # 上述改动的标准 diff
│   ├── apply.sh                 # 一键落盘脚本
│   └── README.md                # 适配详细说明（先读这个）
├── .claude/skills/              # 沉淀的 AI 开发 Skill
│   └── nuttx-audio-lowerhalf-porting/
├── logs/                        # AI Coding 日志
├── app/  quickapp/  board/      # 组委会脚手架
└── contest2026_434_TinyMind.xml # manifest（已增补音频驱动的 linkfile）
```

---

## 四、快速开始

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

# 4) 烧录（产物 out/sifli_sf32lb52_devkit_lcd_nsh/nuttx.bin）
sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 \
       --before default_reset --after soft_reset \
       write_flash <nuttx.bin 绝对路径>@0x12010000
```

验证：串口（1000000 8N1）进 NSH 后执行 `ls /dev`，应看到 **`audio0`**。

详细的编译/烧录/验证步骤与排查表见 [`board/sf32lb52_audio/README.md`](board/sf32lb52_audio/README.md)。

---

## 五、完成度（诚实说明）

| 阶段 | 内容 | 状态 |
|------|------|------|
| M1 | 框架接入：`audio_ops_s` 函数表、`/dev/audio0` 注册 | ✅ 完成 |
| M2 | 硬件上电：电源、时钟表、路由、防爆音时序 | ✅ 完成 |
| M3 | 数据流：`enqueuebuffer` 挂队 + 环形 DMA + 半满/全满中断 + `upper(DEQUEUE)` 回调 | ✅ **代码完成，编译验证通过** |

### 真机验证结果（SF32LB52-DevKit-LCD）

| 证据 | 结果 |
|------|------|
| `ls /dev` | 出现 **`audio0`** |
| `nxrecorder` 串口日志 | `capture config: 48000Hz 2ch 16bit` → `capture started (staging 16384 B, circular)` → `stop: overruns=0` |
| 录音文件 | **262,144 字节** PCM |
| 音频内容 | 峰值 16360 / RMS 356.4，12.7 万样本中 12.6 万非零，波形平滑连续 → ADC 在真实转换模拟输入 |
| `stop` 之后 | 板子仍正常响应（`uname -a` 返回正常） |

**尚未验证**：放音链路（驱动侧已实现并通过编译与符号验证，但因缺 4Ω 喇叭未做实机出声测试）；
另有约 1/3 概率出现一次 `stop` 后无响应的偶发卡死，根因尚未定位。详见技术报告 3.5.4 与 3.7.3。
本仓不含未经实测的性能数据。

---

## 六、技术要点

1. **上下半身分离**：`sf32lb52_audio_s` 首成员为 `struct audio_lowerhalf_s`，
   上层仅持有"基类"指针，各回调强转回完整结构体 —— NuttX 标准 lower-half 模式。
2. **双物理器件**：AUDCODEC 负责模拟↔数字转换（PGA/ADC/DAC），AUDPRC 负责数字路由/混音/EQ，分别配置。
3. **时钟表照抄厂商**：16K/48K 两族由 48MHz XTAL 分频，Σ-Δ 的 OSR 与 chop 配比直接沿用
   SiFli-SDK 验证过的表；表外采样率明确返回 `-EINVAL`，不做无法兑现的能力宣称。
4. **防爆音时序**：放音严格 `静音 → 数字使能 → 模拟分步上电 → 10ms → 开 PA → 100ms → 解除静音`，
   停止严格逆序；录音为 `模拟先上 → MICBIAS 稳定 → 数字使能 → DMA 最后`。
   （爆音与音频数据无关，只能靠模拟时序消除。）
5. **工程化交付**：4 个生产树改动提供 `apply.sh`（文件覆盖式、幂等）+ `patches/`（标准 diff）双形式，
   驱动源文件经 manifest `<linkfile>` 自动就位，**生产仓新增代码零侵入**。

---

## 七、AI-Native 开发

- 全程使用 AI 编程工具（Claude Code）完成驱动适配、构建问题排查与文档撰写
- 沉淀 Skill：[`.claude/skills/nuttx-audio-lowerhalf-porting/`](.claude/skills/nuttx-audio-lowerhalf-porting/SKILL.md)
  —— 把"新芯片音频 lower-half 移植"的方法论与踩坑经验固化为可复用能力，
  并附带"非 git 工作树如何用 mtime 审计出全部改动文件"的实用技巧
- AI Coding 日志见 `logs/`

---

## 八、团队分工

| 成员 | 职责 |
|------|------|
| Issaclfl | 驱动实现、构建集成、技术文档 |
| 队友 | 演示视频、实物照片、海报 |

---

## 九、许可

Apache License 2.0。详见各源文件头部声明。
