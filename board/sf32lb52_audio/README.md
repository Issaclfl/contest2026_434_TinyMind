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

## 三、编译、烧录与验证

```bash
# 1) 拉取工程（组委会提供）
repo init -u https://github.com/open-vela/contest2026_434_TinyMind \
  -b dev-ai-contest-2026 -m contest2026_434_TinyMind.xml
repo sync -c -j8

# 2) 落盘 4 个生产树改动（驱动源文件由 manifest 的 <linkfile> 自动就位）
bash contest2026_434_TinyMind/board/sf32lb52_audio/apply.sh

# 3) 编译（openvela 标准流程：envsetup + lunch + m）
source build/envsetup.sh
lunch vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/nsh
m -j8
# 产物：out/sifli_sf32lb52_devkit_lcd_nsh/nuttx.bin

# 4) 烧录（ROM bootloader + XIP 镜像 @ 0x12010000，无独立 bootloader）
sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 \
       --before default_reset --after soft_reset \
       write_flash out/sifli_sf32lb52_devkit_lcd_nsh/nuttx.bin@0x12010000

# 5) 串口控制台（UART1，1000000 8N1，关流控）
#    注意：本板 RTS 接 VCC 负载开关，screen / cu 会让芯片一直停在复位态
picocom -b 1000000 --noreset --lower-rts --lower-dtr /dev/ttyUSB0
```

### 验证成功的标志

```text
nsh> ls /dev
/dev:
 adc0      buttons   config0   console   fb0       gpio0     gpio1
 audio0    gpio2     i2c0      input0    lcd0      pwm0      ram0
 rtc0      spi1      timer0    ttyACM0   ttyS0     ttyS1     urandom
 watchdog0
```

`audio0` 出现 = M1（框架接入）在真机生效。

进一步验证 M2（硬件真的被配置）：

```text
nsh> nxrecorder
nxrecorder> open
nxrecorder> record
```

驱动会打印硬件配置与上电时序日志（`capture config: 16000Hz 1ch 16bit`、
`capture started (dma=gated)`），证明 `configure()`/`start()` 已作用于 AUDCODEC/AUDPRC 寄存器。
数据通路接通前（M3 未完成），录音取数会以 `-ENOSYS` 干净失败——这是预期行为。

> **烧录常见问题**：`Failed to connect to the chip` 说明 SoC 错过了 RTS 复位后约 2 秒的
> `ATSF32` 监听窗口，重新插拔 USB 或按板载 Reset 键重试。


---

## 四、对生产仓的改动（共 4 处，全部可审计）

设计原则：**新增代码不侵入生产仓**。4 个新文件通过 manifest 的 `<linkfile>` 自动就位；
仅以下 4 个既有文件必须改动，改动内容见 `patches/`：

| 文件 | 改动 | 原因 |
|------|------|------|
| `src/CMakeLists.txt` | `SRCS` 追加 `bsp_audio.c bsp_audio_hw.c` | 让驱动进编译 |
| `drivers/CMakeLists.txt` | 新增 `target_compile_options(... -w)` | 厂商 HAL 头（`bf0_hal_audcodec.h`）含无原型声明，新版工具链 `-Werror` 下编译失败 |
| `configs/nsh/defconfig` | 新增 `CONFIG_AUDIO=y`、`CONFIG_SYSTEM_NXRECORDER=y` | 启用音频框架；并启用 NuttX 自带 `nxrecorder` 作为真机验证工具 |
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

### 6.1 已实测验证的结果

在 `dev-ai-contest-2026` 分支、`arm-none-eabi-gcc 13.2.1` 下，全新配置完整构建（含 M3）：

```text
#### build completed successfully ####

Memory region         Used Size  Region Size  %age Used
           flash:     1451316 B        16 MB      8.65%
            sram:       88672 B       512 KB     16.91%
           psram:           0 B         8 MB      0.00%
```

驱动符号确认已链接进固件（`out/sifli_sf32lb52_devkit_lcd_nsh/System.map`）：

| 符号 | 含义 |
|------|------|
| `sf32lb52_audio_initialize` | 驱动实例化入口 |
| `sf32lb52_getcaps` / `sf32lb52_configure` | 能力查询 / 格式配置回调 |
| `sf32lb52_audio_hw_init` | 硬件上电入口 |
| `audio_register` | NuttX 设备注册 |
| `g_stage`（.bss @ `2000f080`） | 16 KiB 双半 DMA 暂存区（2 × 8 KiB） |
| `sf32lb52_serve_half` | 半区数据搬运（中断上下文） |
| `sf32lb52_stage_callback` | 中断→上层回调桥接 |
| `sf32lb52_prime_playback` | 放音启动前预填暂存区 |
| `hw_rx_dma_isr` / `hw_tx_dma_isr` | AUDPRC DMA 中断服务 |
| `HAL_AUDPRC_RxCpltCallback` 等 4 个 | HAL 弱回调覆盖（全满/半满 × 收发） |
| `nxrecorder_main` | 真机验证工具（NuttX 自带） |

### 6.2 阶段完成度

| 阶段 | 内容 | 状态 |
|------|------|------|
| M1 | 框架接入：函数表 + `/dev/audio0` 注册 | ✅ 完成，编译验证 |
| M2 | 硬件上电：电源/时钟/路由/防爆音时序 | ✅ 完成，编译验证 |
| M3 | 数据流：`enqueuebuffer` 挂队 + 环形 DMA + 半满/全满中断 + `upper(DEQUEUE)` 回调 | ✅ **代码完成，编译验证通过** |

**M3 实现要点**（详见下文第七节）：

- 采用**私有双半暂存区**（2 × 8 KiB）承载环形 DMA，一次中断交付/取用一个上层缓冲；
  这样把 DCache 一致性、缓冲对齐、环形模式三个问题全部收敛到一块自有缓冲上。
- DMA 走 **circular 模式 + 半满/全满双中断**——不是随意选择，而是 HAL 依据
  `dest_sel != AUDPRC_TX_TO_MEM` 自行决定的模式（`bf0_hal_audprc.c:796`），
  代码里显式设置 `dest_sel` 并加注释说明，避免日后被误改。
- 采集方向在每个半区交付前 `up_invalidate_dcache()`，放音方向在回填后
  `up_clean_dcache()`；启动前对整块暂存区 `up_flush_dcache()`。
- `stop()` 会把 `pendq` 中尚未服务的缓冲全部以 `nbytes = 0` 交还上层，
  否则 `nxrecorder` 会一直等不可能完成的缓冲。
- 中断里只做一次有界 memcpy 加上层回调（NuttX 明确允许上层回调在中断上下文调用）。

**尚未验证**：以上均为编译期与符号级验证。**真机行为（能否真正录到音、放音是否出声）
尚未实测**，需上板后按 `docs/上板验证清单.md` 走一遍。本仓不含未经实测的性能数据。
