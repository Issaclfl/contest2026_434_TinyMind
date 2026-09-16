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
           flash:     1451360 B        16 MB      8.65%
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

---

## 七、适配难点与修复记录

本节记录适配过程中**实际踩到并修掉**的问题。它们大多属于"编译能过、上板才炸"的类型，
也是本项目最值得复用的经验。

### 7.1 设备注册路径：传了完整路径，设备根本没注册上

**现象**：编译通过、符号齐全，但真机上 `ls /dev` 永远没有音频设备。

**根因**：`audio_register()` 只接受设备**名**，NuttX 内部拼成 `/dev/audio/[name]`
（`nuttx/audio/audio.c:1793-1835`，官方注释见 `:1926-1929`）。传入完整路径
`"/dev/audio0"` 时算出的路径是 `/dev/audio//dev/audio0`；而 `inode_reserve()`
遇到中间目录不存在会**直接返回 `-ENOENT`**（`nuttx/fs/inode/fs_inodereserve.c:243-246`），
注册必然失败。

**修复**：改用 NuttX 为这种情况提供的 "simple case"
（`CONFIG_AUDIO_CUSTOM_DEV_PATH=y` + `CONFIG_AUDIO_DEV_ROOT=y`，定义见
`nuttx/audio/Kconfig:219,229`）——路径变为 `"/dev/" + name`，得 `/dev/audio0`，
且无需预先创建目录。调用改为传裸名：`audio_register("audio0", audio)`。

**经验**：NuttX 里凡是 `xxx_register(name, ...)` 形式的接口，先确认它要的是
"名字"还是"路径"。名字型的接口传错路径往往不报错，而是静默错位。

### 7.2 `cat /dev/audio0` 是无效的验证方法

**现象**：`cat /dev/audio0` 立刻返回、无输出、无报错，看起来"设备正常"。

**根因**：NuttX 音频上半身的 `audio_read()` 是空壳——`ops->read == NULL` 时直接
`return 0`（`nuttx/audio/audio.c:305-310`），表现为立即 EOF。数据通路根本不经过
`read()`，而走 ENQUEUEBUFFER / DEQUEUE 回调 + 消息队列。

**修复**：验证一律使用 `nxrecorder`（走 RESERVE → GETCAPS → CONFIGURE →
GETBUFFERINFO → ENQUEUEBUFFER → START 完整流程）。另注意：未 CONFIGURE 就
START 会返回 `-EPERM`（`audio.c:621-624`）。

**经验**："不报错"不等于"成功"。对这种空壳接口，必须用会真正走完整流程的工具验证。

### 7.3 `enter_critical_section()` 在非 SMP 配置下链不出来

**现象**：编译通过，链接失败——`undefined reference to enter_critical_section`。

**根因**：`nuttx/include/nuttx/spinlock.h:1538-1542` 的守卫是
`#if CONFIG_SCHED_CRITMONITOR_MAXTIME_CSECTION >= 0 || defined(...INSTRUMENTATION_CSECTION)`。
该配置项在本板**未定义**，预处理时按 0 参与比较，`0 >= 0` 为真，于是
`enter_critical_section` 被声明为**外部函数**；而它的实现
（`sched/irq/irq_csection.c`）只在 SMP 下编译。

**修复**：单核板级驱动改用体系结构原语 `up_irq_save()` / `up_irq_restore(flags)`。

**经验**：报"未定义"时，先分辨是"我写错了"还是"这个符号在本配置下本就不存在"。

### 7.4 `dq_rem()` 是语句宏，不能当表达式用

**现象**：编译报 `expected expression before 'do'`。

**根因**：`dq_rem()` 是 `do { ... } while (0)` 形式的宏（`nuttx/include/nuttx/queue.h:229`），
无法出现在赋值右侧。

**修复**：先用同为宏但会求值的 `dq_inqueue()`（`queue.h:325`）判断，再调用 `dq_rem()`。

### 7.5 改 defconfig 后配置不生效（构建系统陷阱）

**现象**：改了 defconfig，`grep CONFIG_XXX out/<board>_<config>/.config` 仍是
`not set`，且**产物大小与改动前完全一致**。

**根因**：增量构建复用 `out/.../.config`，**不会重新读取 defconfig**。
判断依据：`.config` 的时间戳停留在上一次配置的时间。

**修复**：删除 `out/<board>_<config>` 后重新 `lunch` + `m`。

**经验**：核对"产物大小/行数是否变化"，是识破"编译成功但跑的是旧代码"的唯一廉价手段。

### 7.6 工具链与 kconfiglib 不在非交互 shell 的 PATH 里

| 缺失项 | 报错 | 位置 |
|--------|------|------|
| ARM 工具链 | `/bin/sh: 1: arm-none-eabi-ar: not found`（链接期才报） | `~/arm-tc-root/usr/bin` |
| kconfiglib 的 `olddefconfig` | `FATAL_ERROR: Kconfig environment depends on kconfiglib`（模块其实已装，是 PATH 问题） | `~/.local/bin` |

**修复**：脚本里显式 `export PATH="$HOME/arm-tc-root/usr/bin:$HOME/.local/bin:$PATH"`。

### 7.7 真机调试阶段发现的四个缺陷（全部编译期不可见）

以上 7.1–7.6 是工程环境与构建层的问题。下面四个是**上板后才暴露的驱动逻辑缺陷**，
每一个都会让"编译通过的固件"在真机上以完全不同的方式失败。

#### (1) 开机静默死住 —— 最严重

**现象**：烧录成功、回读校验逐字节一致，但板子**一个字都不输出**；换波特率、换
RTS/DTR 电平组合、按 Reset、长监听，全部零字节。对照实验（把音频初始化整段移除
后重新烧录）立刻正常启动到 `nsh>`，从而把范围锁定在音频初始化。

**根因**：`sf32lb52_audio_hw_init()` 把 `memset` 过的 codec DMA 句柄挂进了
`g_audcodec.hdma[]`。`HAL_AUDCODEC_Init()` 对非 NULL 的句柄调用
`HAL_AUDCODEC_DMA_Init()` → `HAL_DMA_Init()`，而后者第一步就是

```c
HAL_ASSERT(IS_DMA_ALL_INSTANCE(hdma->Instance));   /* bf0_hal_dma.c:550 */
```

`Instance` 是 NULL，断言失败。**本 HAL 的 `HAL_ASSERT` 无论是否定义
`USE_FULL_ASSERT`，展开都是 `while (1) {;}`（`bf0_hal.h:419`/`:421`）**——
不打印、不复位、不报错，就是永久空转。

而 HAL 里 `if (hacodec->hdma[i] != NULL)` 那个守卫，本来就是为"未使用的通道留
NULL"设计的。

**修复**：codec DMA 通道保持 NULL，不挂任何句柄（音频数据始终走 AUDPRC）。

**经验**：`xxx_Init()` 里的循环守卫（`!= NULL`）往往就是"哪些字段可以不填"的
官方说明。主动去"补全"它，可能正好踩进断言陷阱。而这类断言的失败方式是
**静默死循环**，没有任何日志可依——只能用"逐步插打印"二分定位。

#### (2) `HAL_AUDCODEC_Config_RChanel()` 的通道号约定

**现象**：录音时打印 `codec Config_RChanel failed: 1`（`HAL_ERROR`），采集永远配不起来。

**根因**：该函数内部是 `switch (channel) { case 0: ... case 1: ... default: return HAL_ERROR; }`，
写的是 `ADC_CH0_CFG` / `ADC_CH1_CFG`——**它要的是"方向内的相对通道号 0/1"**，
方向由调用哪个函数（RChanel / TChanel）决定。而 `HAL_AUDCODEC_ADC_CH0` 的枚举值是
**0x02**，直接落进 `default`。

（`Config_TChanel` 同样按 0/1 分支，而 `HAL_AUDCODEC_DAC_CH0` 恰好是 0，所以放音
路径"碰巧"是对的——这种一半对一半错的情况最容易漏掉。）

**修复**：两处都传 `0`。

**经验**：HAL 里的通道参数有两种约定（全局枚举 vs 方向内相对号），
**只能以其 `switch` 分支的实际写法为准**，不能按枚举名想当然。

#### (3) 设备关闭后永久 busy

**现象**：`nxrecorder` 里 `device /dev/audio0` 之后再录音，报 `Audio device busy`。

**根因**：`shutdown()`（上半身在最后一个句柄关闭时调用）只拆了硬件，没有清理
`reserved` / `running` / `configured`。`reserved` 一旦置位就再无人清，之后任何
`reserve()` 都返回 `-EBUSY`。

**修复**：在 `shutdown()` 里复位这三个状态位（这是唯一可靠的会话结束钩子）。

#### (4) `stop` 之后整块板卡死

**现象**：录音日志一切正常（`overruns=0`），但 `stop` 之后板子再无任何响应，
连 `uname -a` 都不回。

**根因**：上半身的 `audio_stop()` 会**先把会话置为 `AUDIO_STATE_DRAINING`，
再调用下半身的 `stop()`**（`audio.c:692`），而只有 `AUDIO_CALLBACK_COMPLETE`
能把它转回 `OPEN`（`audio.c:1558`）。主机侧同样在等这个信号：`nxrecorder` 的
录音线程收到 `AUDIOIOC_STOP` 后**故意不退出循环**，"we will loop until
AUDIO_MSG_COMPLETE is received"（`nxrecorder.c:804`），只有
`AUDIO_MSG_COMPLETE` 才让它 `running = false`。

**修复**：`stop()` 清空 `pendq` 后主动上报 `AUDIO_CALLBACK_COMPLETE`。

**经验**：NuttX 音频框架的 `stop` 是一次**双向握手**——下半身必须回一个
"complete" 才算结束。只停硬件不回消息，主机线程会永远等下去，表现为整机假死。

#### 真机验证结果

修复后在同一块 SF32LB52-DevKit-LCD 上（`sftool` 烧录至 `0x12010000`）：

| 证据 | 结果 |
|------|------|
| `ls /dev` | 出现 **`audio0`** |
| `nxrecorder` 串口日志 | `capture config: 48000Hz 2ch 16bit` → `capture started (staging 16384 B, circular)` → `stop: overruns=0` |
| 录音文件 | **270,336 字节**（33 个 8 KiB 缓冲） |
| 文件内容 | `hexdump` 显示采样值平滑连续（16106→…→2000 余），**非全零、非乱码** → ADC 在真实转换模拟输入 |
| `stop` 之后 | 板子仍正常响应（`uname -a` 正常返回） |

