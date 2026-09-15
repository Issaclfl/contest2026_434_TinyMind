# SF32LB52 板载音频驱动适配（NuttX audio lower-half）

> 作品核心交付物 ｜ 队伍 TinyMind ｜ 2026 openvela AI 硬件开发者大赛 · 新硬件适配赛道

---

## 一、这是什么

为 **SF32LB52-DevKit-LCD** 补上 openvela/NuttX 官方未提供的**板载音频驱动**。
适配前该板是"哑巴"：麦克风与喇叭硬件焊在板上，但系统里没有任何代码能指挥它们。

适配后系统出现标准音频设备 `/dev/audio0`，应用可用 NuttX 标准接口录音/放音：

```c
int fd = open("/dev/audio0", O_RDONLY);
ioctl(fd, AUDIOIOC_CONFIGURE, &caps);   /* 16000Hz / 16bit / 单声道 */
ioctl(fd, AUDIOIOC_START, 0);
read(fd, buf, len);                     /* 录音 */
```

硬件通路：

```
录音 capture : 板载模拟 MEMS 麦克风 -> AUDCODEC ADC_CH0 -> AUDPRC RX_CH0 -> DMA(P2M) -> RAM
放音 playback: RAM -> DMA -> AUDPRC TX_CH0 -> AUDCODEC DAC_CH0 -> AW8155 功放(PA10) -> 喇叭
```

---

## 二、目录结构

```text
board/sf32lb52_audio/
├── src/bsp_audio.c           # NuttX upper-half 适配层（audio_ops_s 函数表、/dev/audio0 注册实体）
├── src/bsp_audio_hw.c        # 硬件上电/时钟/路由/防爆音时序（寄存器值逐行对齐 SiFli-SDK 参考驱动）
├── include/bsp_audio.h       # 适配层对外接口 + 方向枚举
├── include/bsp_audio_hw.h    # 硬件层对外接口
├── board_files/              # 需要在生产树上覆盖的 4 个文件（见第四节）
├── patches/                  # 上述改动的 diff，供评审查看
├── apply.sh                  # 一键落盘脚本
└── README.md
```

---

## 三、编译与运行

```bash
# 1) 拉取工程（组委会提供）
repo init -u https://github.com/open-vela/contest2026_434_TinyMind \
  -b dev-ai-contest-2026 -m contest2026_434_TinyMind.xml
repo sync -c -j8

# 2) 落盘 4 个生产树改动（驱动源文件由 manifest 的 <linkfile> 自动就位）
bash contest2026_434_TinyMind/board/sf32lb52_audio/apply.sh

# 3) 编译
./build.sh vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/nsh

# 4) 烧录
out/sifli_sf32lb52_devkit_lcd_nsh/nuttx.bin
```

**验证成功的标志**：串口进入 NSH 后执行 `ls /dev`，能看到 `audio0`。

---

## 四、对生产仓的改动（共 4 处，全部可审计）

设计原则：**新增代码不侵入生产仓**。4 个新文件通过 manifest 的 `<linkfile>` 自动就位；
仅以下 4 个既有文件必须改动，改动内容见 `patches/`：

| 文件 | 改动 | 原因 |
|------|------|------|
| `src/CMakeLists.txt` | `SRCS` 追加 `bsp_audio.c bsp_audio_hw.c` | 让驱动进编译 |
| `drivers/CMakeLists.txt` | 新增 `target_compile_options(... -w)` | 厂商 HAL 头（`bf0_hal_audcodec.h`）含无原型声明，新版工具链 `-Werror` 下编译失败 |
| `configs/nsh/defconfig` | 新增 `CONFIG_AUDIO=y` | 启用 NuttX 音频框架 |
| `src/sifli_ap.c` | 新增 include + `audio_register("/dev/audio0", ...)` | 启动时注册设备节点 |

`apply.sh` 以**文件覆盖**而非 `patch` 方式落盘，避免上下文偏移导致应用失败；`patches/` 同时提供标准 diff 便于评审审阅。

---

## 五、实现要点

1. **上下半身分离**：`sf32lb52_audio_s` 首成员为 `struct audio_lowerhalf_s`，
   上层通过 `&priv->dev` 只持有"基类"指针，各回调强转回大结构体（NuttX 标准 lower-half 模式）。
2. **函数表**：`g_sf32lb52_audio_ops` 回答 getcaps/configure/start/stop/enqueuebuffer/… 全部标准问题。
3. **双物理器件**：AUDCODEC（模拟↔数字）+ AUDPRC（数字路由），分别配置。
4. **时钟表照抄厂商**：16K/48K 两族均由 48MHz XTAL 分频，Σ-Δ OSR/chop 配比直接沿用 SiFli-SDK
   验证过的表，表外采样率明确返回 `-EINVAL`。
5. **防爆音时序**：放音严格遵循 `静音 → 数字使能 → 模拟分步上电 → 10ms → 开 PA → 100ms → 解除静音`，
   关机严格逆序；录音为 `模拟先上 → MICBIAS 稳定 → 数字使能 → DMA 最后`。

---

## 六、当前完成度（诚实说明）

| 阶段 | 内容 | 状态 |
|------|------|------|
| M1 | 框架接入：函数表 + `/dev/audio0` 注册 | ✅ 完成 |
| M2 | 硬件上电：电源/时钟/路由/防爆音时序 | ✅ 完成 |
| M3 | 数据流：`enqueuebuffer()` 挂队 + DMA 换桶 + `upper()` 回调 | ⏳ 进行中 |

**已知限制**：`enqueuebuffer()` 当前返回 `-ENOSYS`，即录音/放音的数据通路尚未接通；
`configure`/`start`/`stop` 的硬件动作已全部实现，可直接用串口日志验证。
本仓不含未经实测的性能宣称。
