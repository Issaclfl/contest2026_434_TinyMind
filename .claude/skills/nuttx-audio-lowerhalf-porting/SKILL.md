---
name: nuttx-audio-lowerhalf-porting
description: 在 openvela/NuttX 上为一颗新芯片或新板子移植板载音频（codec）驱动时使用。覆盖 audio lower-half 函数表实现、AUDCODEC/AUDPRC 双器件配置、时钟表照抄厂商参考驱动、防爆音上电时序、以及"非 git 工作树如何找出全部改动文件"的审计方法。当用户提到"音频驱动移植""audio lower-half""/dev/audio0""codec 上电时序""爆音""AUDCODEC""AUDPRC""enqueuebuffer 数据流"时应触发。
---

# NuttX 音频 lower-half 驱动移植

把一块"哑巴"板子（有麦克风/喇叭硬件但系统无驱动）接到 NuttX 标准音频框架上。

## 核心心智模型：四层结构

```
应用  read("/dev/audio0")
  ↓
NuttX 音频上半身（官方，不改）—— 排队/缓冲/多应用仲裁
  ↓  dev->ops->xxx()
 适配层 lower-half（你写）—— 回答标准问题，翻译成硬件动作
  ↓
 硬件层（你写）—— 寄存器值 + 等待时间 + 顺序
  ↓
厂商 HAL / 寄存器（芯片厂提供）
```

**你的全部工作 = 中间两层。** 不要试图改上半身。

---

## 步骤 1：先找厂商参考驱动，不要发明寄存器值

在写任何寄存器之前，先在厂商 SDK 里找到**已经验证过**的音频驱动：

```bash
# SiFli 为例
find <sdk>/drivers -name 'drv_aud*' -o -name '*audcodec*' -o -name '*audprc*'
```

**必须照抄的三类东西**：
1. **时钟配置表**（采样率 → 分频器/OSR/chop 配比）——Σ-Δ ADC/DAC 的 OSR 与 chop 时钟直接决定噪声和直流失调，厂家调好的表不要重算。
2. **上电/下电顺序**——见步骤 3。
3. **默认路径配置**（mixer/mux 选谁、EQ 开关、增益默认值）。

**反模式**：凭 datasheet 自己推分频比 → 结果通常是"无声"或"噪声很大"，且不会报错，极难调。

## 步骤 2：定义状态结构体（C 版继承）

```c
struct myboard_audio_s
{
  struct audio_lowerhalf_s dev;   /* 必须是第一个成员 */
  uint32_t samprate;              /* 以下才是自己的状态 */
  uint8_t  nchannels;
  bool     configured;
  bool     running;
};
```

- `initialize()` 返回 `&priv->dev`（只交出"基类"地址）
- 每个回调第一件事：`FAR struct myboard_audio_s *priv = (FAR struct myboard_audio_s *)dev;`
- **dev 放第二个成员 = 强转后字段全部错位**（不崩溃，读垃圾，最难查的 bug）

## 步骤 3：填入 audio_ops_s 函数表

参照同目录下的 `audio_null.c` 确定每个回调的填空约定：

```c
static const struct audio_ops_s g_ops =
{
  .getcaps    = xxx_getcaps,     /* 你能干嘛 */
  .configure  = xxx_configure,   /* 绑定采样率/声道/位宽 */
  .start      = xxx_start,
  .stop       = xxx_stop,
  .enqueuebuffer = xxx_enqueue,  /* 数据流通路，见步骤 5 */
  .cancelbuffer  = xxx_cancel,
  .ioctl      = xxx_ioctl,       /* 至少处理 AUDIOIOC_GETBUFFERINFO */
  .reserve    = xxx_reserve,     /* 独占标志 */
  .release    = xxx_release,
};
```

**getcaps 填空要点**：`AUDIO_TYPE_QUERY` 时回答 `ac_controls.b[0] = AUDIO_TYPE_INPUT`（或 OUTPUT）；
`AUDIO_TYPE_INPUT` 时回答支持的采样率位掩码。**只上报时钟表里真有的采样率**——上报了但 configure 拒绝，是自相矛盾的接口。

**GETBUFFERINFO 决定缓冲策略**：`buffer_size` 按"每桶多少毫秒音频"反算。16kHz/16bit/单声道 = 32 B/ms，
8 KiB 桶 ≈ 256 ms，通常给 2 个桶做双缓冲。

## 步骤 4：防爆音上电时序（放音必做）

爆音的物理本质：功放输出端偏置靠电容建立，使能瞬间若停在偏离共模点的直流电位，喇叭收到直流阶跃 → "砰"。
**爆音与数据无关**（一个字节都没发也会砰），所以只能靠模拟时序防。

放音启动严格顺序：

```
静音 → DMA 挂上 → 数字使能 → 模拟分步上电 → 等 10ms → 开 PA → 等 100ms → 解除静音
```

放音停止**严格逆序**（下电的直流跳变同样会炸）：

```
关 PA → 静音 → 模拟分步下电 → 清通道 → 数字复位
```

录音顺序不同（无爆音问题，只需避免把上电毛刺录进数据）：

```
模拟先上 → MICBIAS 稳定 ~20ms → 数字使能 → DMA 最后
```

**PA 使能脚常有额外要求**：本项目的 AW8155 要求先拉低 550µs 再拉高一次以选默认增益档——这类细节只能从厂商驱动抄。

## 步骤 5：数据流通路（enqueuebuffer）

```c
/* 上半身交来一个空桶 */
int xxx_enqueuebuffer(dev, apb)
{
  入队 apb 到 pendq;
  if (DMA 空闲)  HAL_xxx_Receive_DMA(handle, apb->samp, apb->nmaxbytes);
  return OK;
}

/* DMA 完成（中断或轮询） */
{
  出队 apb;
  dev->upper(dev->priv, AUDIO_CALLBACK_DEQUEUE, apb, OK);  /* 交还上半身 */
  若 pendq 还有桶 → 启动下一次 DMA
}
```

`dev->upper` 是上半身在注册时塞进来的回调，**方向是下层→上层**，别和 `dev->ops`（上层→下层）搞混。

**常见坑**：DMA 描述符要按缓存行对齐；`apb->nmaxbytes` 是有效长度；多桶乒乓时不要在 DMA 还在写的桶上回调。

## 步骤 6：注册设备

在板级 bringup 里：

```c
FAR struct audio_lowerhalf_s *audio = myboard_audio_initialize();
audio_register("/dev/audio0", audio);
```

验证：串口 `ls /dev` 能看到 `audio0`。

---

## 附一：构建环境三个坑（都会让编译直接失败）

openvela 的 CMake 配置阶段对 PATH 很敏感，而项目常把工具链装在工作区之外：

1. **ARM 工具链**：`find_program` 找不到时只在链接期报
   `/bin/sh: 1: arm-none-eabi-ar: not found`——**报错很晚，容易误判成代码问题**。
   先 `which arm-none-eabi-gcc arm-none-eabi-ar` 确认。

2. **kconfiglib 的 `olddefconfig`**：`nuttx/CMakeLists.txt` 用
   `find_program(KCONFIGLIB olddefconfig)` 检测，找不到就 `FATAL_ERROR`。
   该可执行文件由 `pip install kconfiglib` 装到 `~/.local/bin`，
   **而 `~/.local/bin` 常不在 PATH 里**。报错文案只说"请安装 kconfiglib"，
   但模块其实已装好——是 PATH 问题，容易误诊。

3. **非交互 shell / `bash script.sh` 不加载 `.bashrc`**：上述两个路径都靠 `.bashrc` 提供，
   批量脚本里必须显式 `export PATH=...`。

```bash
export PATH="$HOME/.local/bin:$HOME/<toolchain-root>/bin:$PATH"
which arm-none-eabi-gcc arm-none-eabi-ar olddefconfig
```

## 附二：改 Kconfig/defconfig 后配置不生效

**症状**：改了 defconfig，`grep CONFIG_XXX out/<board>_<config>/.config` 仍是
`not set`，且产物大小与改动前**完全相同**。

**原因**：增量构建复用 `out/.../.config`，**不会重新读取 defconfig**。

**处理**：删掉构建目录后重新配置。

```bash
rm -rf out/<board>_<config>
lunch <board-config-path>
m -j8
```

**验证配置真的生效**（不要只看编译成功）：

```bash
grep CONFIG_<你的符号> out/<board>_<config>/.config
grep -c "<你的函数名>" out/<board>_<config>/System.map   # 符号进没进固件
```

后者尤为关键：它证明**你写的代码真的被链接进了固件**，而不只是"编译没报错"。

---

## 附：在非 git 工作树里找出全部改动文件

openvela 的 `repo` 工作区若没有 `.repo`（或不是 git 仓），无法用 `git status` 审出改动。
用 **mtime 与同步日期对比** 是好办法：

```bash
# 同步日期通常是所有 pristine 文件的 mtime；之后被改的文件会更新
find <work> -type f -newermt '2026-09-01' \
  ! -path '*/out/*' ! -path '*/build/*' ! -path '*/.git/*' \
  -printf '%TY-%Tm-%Td %TH:%TM  %p\n' | sort
```

**排除噪声**：构建过程会触碰部分文件（如 `.cargo/config.toml`、某些 libc 文件）的 mtime，
需逐个 `grep` 确认内容里是否真有自研改动，不要凭时间戳就认定是自己的改动。

## 提交前的审计清单

- [ ] 新增源文件**没有**散落在生产树里没人管（应在自己的仓中，经 manifest `<linkfile>` 就位）
- [ ] 列出**所有**被改的生产树文件（含 CMakeLists、defconfig、bringup 文件）——漏一个就编不出固件
- [ ] 改动若导致厂商头文件在新工具链下 `-Werror` 失败，需为厂商代码单独关警告，**不要**全局关
- [ ] 提供 `apply.sh`（文件覆盖式，幂等）而非只给 patch——避免上下文偏移导致应用失败
- [ ] 同时提供 `patches/` 标准 diff 供评审审阅
- [ ] 未实测的性能数据不要写进报告
