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
| `configs/nsh/defconfig` | 新增 `CONFIG_AUDIO=y`、`CONFIG_SYSTEM_NXPLAYER=y`、`CONFIG_SYSTEM_NXRECORDER=y` | 启用音频框架；并启用 NuttX 自带的 `nxrecorder` / `nxplayer` 作为真机验证工具（`ai_agent` 配置同样处理，见 `ai_agent_port/`） |
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
- `stop()` 对两个方向区别对待：**采集**把 `pendq` 里未服务的缓冲直接丢掉（上层即将退出，
  自己会用 `AUDIOIOC_FREEBUFFER` 释放；再交还一次会让它把录音尾巴写第二遍）；**放音**必须
  以 `nbytes = 0` 交还（播放器自己记着"还没看到归还的缓冲数"，在 `COMPLETE` 时对账）。
- 中断里只做一次有界 memcpy 加上层回调（NuttX 明确允许上层回调在中断上下文调用）；
  播放流收到带 `AUDIO_APB_FINAL` 的缓冲后，再等两个半区（约一个 DMA 周期、把尾巴真的送出去）
  才交 `AUDIO_CALLBACK_COMPLETE`，避免提前停机切掉结尾。

**验证状态**：M1/M2/M3 均已在真机验证（证据表见 **7.7** 与 **7.8**）。采集方向有 30 秒连续录音、
零丢包、WAV 回放听感与波形统计；放音方向的数字通路已跑通（纯音与 `playraw` 自然播完、
会话自动结束），且**喇叭实际出声已由人耳确认**（2026-09-19：先听到 1 kHz 纯音、随后听到
录音回放，两段可分辨）。音量大小、失真程度、上电/断电爆音未做专项测量。

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

#### (4) `stop` 之后整块板卡死（两阶段排查）

**现象**：录音日志一切正常（`overruns=0`），但 `stop` 之后板子再无任何响应，
连 `uname -a` 都不回。约 1/3 的会话必现，**间歇性**。

**第一阶段——缺失的完成信号**

上半身的 `audio_stop()` 会**先把会话置为 `AUDIO_STATE_DRAINING`，再调用下半身的
`stop()`**（`audio.c:692`），而只有 `AUDIO_CALLBACK_COMPLETE` 能把它转回
`OPEN`（`audio.c:1558`）。主机侧同样在等这个信号：`nxrecorder` 的录音线程收到
`AUDIOIOC_STOP` 后**故意不退出循环**，注释写明 "we will loop until
AUDIO_MSG_COMPLETE is received"（`nxrecorder.c:804`），只有 `AUDIO_MSG_COMPLETE`
才让它 `running = false`。

于是在 `stop()` 里补上了 `upper(AUDIO_CALLBACK_COMPLETE)`。

**经验**：NuttX 音频框架的 `stop` 是一次**双向握手**——下半身必须回一个
"complete" 才算结束。只停硬件不回消息，主机线程会永远等下去。

**第二阶段——即使补了信号，仍然会卡**

补上 COMPLETE 后成功率上升，但**偶发卡死依旧存在**（30 秒连续录音那次就复现了）。
继续追下去，真正的根因是**阻塞发送**：

- `AUDIO_CALLBACK_COMPLETE` 最终会走到上半身的 `file_mq_send()`，其优先级取
  `CONFIG_AUDIO_BUFFER_DEQUEUE_PRIO`。该宏在 `nuttx/audio/audio.c:66-68` 的
  **默认值是 1**，而 `nxrecorder` 打开消息队列时**没有 `O_NONBLOCK`**——
  也就是说**队列满时这个发送会阻塞**。
- 而调用现场是：录音线程停在 `AUDIOIOC_STOP` ioctl 内部（即我们的 `stop()` 里），
  CLI 主线程停在 `pthread_join` 上，**没有任何人在排空那个队列**。队列一旦恰好
  是满的，发送永不返回 → 整机假死。这解释了它为什么是间歇性的：取决于那一刻
  队列里恰好堆了多少条待处理的 DEQUEUE 消息。

**最终修复**：把 COMPLETE 投递到**低优先级工作队列**异步发送
（`work_queue(LPWORK, ...)` → `sf32lb52_complete_worker()`）。任务上下文里阻塞
是合法的；`stop()` 得以立即返回，应用随即恢复排空队列。

这正是 NuttX 自己给 lower-half 建议的形态——框架注释写着 enqueuebuffer 可以
"add it to a queue for processing by a background thread or worker task"
（`nuttx/audio/audio.c:1450`）。

**经验**：**在别人的调用路径上同步回调，风险比看起来大**。框架的 `file_mq_send`
默认是阻塞语义，而"谁在排空这个队列"必须逐案推演——如果答案是"没有人在排空"，
那么同步回调就是死锁。把回调挪到自己的工作队列里，能把这类问题一次性消掉。

#### 真机验证结果

修复后在同一块 SF32LB52-DevKit-LCD 上（`sftool` 烧录至 `0x12010000`）：

| 证据 | 结果 |
|------|------|
| `ls /dev` | 出现 **`audio0`** |
| `nxrecorder` 串口日志 | `capture config: 16000Hz 1ch 16bit` → `capture started (staging 16384 B, circular)` → `stop: overruns=0` |
| **30 秒连续录音** | **`overruns=0`（全程零丢包）**，落盘 **1,048,576 字节** |
| 录音文件回传 | 分 32 块经串口搬回、每块校验字节数：**1,048,576 / 1,048,576 完整**（100% 无损） |
| 音频内容 | 峰值 32715 / RMS 285.8，52.4 万样本中 52.1 万非零，波形平滑连续 → ADC 在真实转换模拟输入 |
| 可播放产物 | 导出为 16 kHz 单声道 WAV（32.8 秒），已在 PC 上实际播放验证 |
| `stop` 之后 | 板子仍正常响应（`uname -a` 正常返回），多次会话可重复 |
| **放音：纯音与回环**（2026-09-19 新增） | `nxplayer tone 48000 2 1000` 与 `nxplayer playraw /data/loop.pcm 2 16 48000 0` 都走完 `playback started → final buffer served → end of stream, COMPLETE queued`，会话自动结束；2 秒纯音耗时 **2.4 秒**（速率≈实时）；`gave back 0 queued playback buffer(s)`，应用侧缓冲对账为零；**人耳确认喇叭出声**（先 1 kHz 纯音、后录音回放，两段可分辨） |
| **采集格式对账**（2026-09-19 新增） | 48kHz/2ch 4 秒落盘 **802,816 字节**（≈192 kB/s）、16kHz/1ch 4 秒落盘 **131,072 字节**（≈32 kB/s）；修复前 48kHz/2ch 只有约一半（见 7.8 ②） |

**可听证据**：`docs/audio-evidence/board-capture-16k-mono.wav`（板子录到的真实音频）。

**放音串口完整记录**：`docs/audio-evidence/playback-loop-48k-stereo.log`；
**采集格式对账记录**：`docs/audio-evidence/capture-format-matrix.log`。
**出声人耳确认（2026-09-19）**：按上述命令实测，人耳听到 1 kHz 纯音与随后的录音回放，两段可分辨，
放音链路（DMA → DAC → Class-D 功放 → 喇叭）确认出声。**音量大小、失真程度、上电/断电爆音
未做专项测量**——量化数据只到数字通路这一层。

### 7.8 上板第三轮：放音与采集的两个"静默降级"缺陷（2026-09-19）

接上 4Ω 喇叭跑放音验证时又挖出两个缺陷。它们的共同点是**不报错、不崩溃，只是安静地不工作**，
而且都在厂商 HAL 里同一处：**通道配置只写进 handle 结构体是不够的，寄存器由
`HAL_AUDPRC_Config_{T,R}Chanel()` 写下去**。

#### (1) 放音 DMA 一步都不走

**现象**：`nxplayer` 打印 `playback config` / `playback started` 之后就什么都没有；
`stop: overruns=0`（**连一次半区中断都没来**）；紧接着再播放报 `Audio device busy`；
退出 `nxplayer` 时触发它自己的 `DEBUGASSERT(outstanding == 0)` 崩溃并打印寄存器。

**定位**：三条现象共同指向"传输 DMA 从未前进"。对照厂商 SDK 的 replay 配置路径
（`drv_audprc.c:1018` 显式调用 `HAL_AUDPRC_Config_TChanel(haudprc, 0, &cfg)`）后确认：
我们的 `config_playback()` 只做了 `g_audprc.cfg1 = prc_cfg;`——**那只把结构体存进 handle**。

**根因**：TX 通道保持复位默认值（`AUDPRC_TX_CH0_CFG.ENABLE == 0`）→ AUDPRC 不向 DMA 发请求
→ 无半区中断、无 `DEQUEUE` 回调、无 `COMPLETE`，应用永远停在 PLAYING。
采集方向之所以一直"看起来正常"，是因为 `HAL_AUDPRC_Receive_DMA()` 内部替我们写了 RX 行；
**TX 没有这一步**。

**修复**：`config_playback()` 补 `HAL_AUDPRC_Config_TChanel(&g_audprc, 0, &g_audprc.cfg1)`。
修复后立刻能看到 `stop: overruns=5`（DMA 真在跑），流自然播完。

#### (2) 48kHz/2ch 采集被静默降级成单声道

**现象**：不报任何错，`capture config: 48000Hz 2ch 16bit` 照常打印，但**同一时长下落盘字节数
只有标称的一半**：6 秒只落 589,824 字节——而 589,824 ÷ 6 s ≈ 98 kB/s，**恰好等于"48kHz 单声道"**。

**定位**：把"文件字节数 ÷ 墙钟时长"与"标称字节率"做三组对比：16kHz/1ch 对得上，
48kHz/2ch 差一半。既然 (1) 已经证明"handle 结构体 ≠ 寄存器"，回查采集路径——同样漏了
`HAL_AUDPRC_Config_RChanel()`。

**根因**：RX 通道跑复位默认值（16bit **单声道**），宿主请求的 2ch 从未写进硬件。
所谓"48kHz 立体声录音"其实是单声道数据被按双声道解析。

**修复**：`config_capture()` 补 `HAL_AUDPRC_Config_RChanel(&g_audprc, 0, &g_audprc.cfg)`。
修复后 48kHz/2ch 4 秒落盘 802,816 字节（≈192 kB/s），与 16kHz/1ch 的 131,072 字节（≈32 kB/s）
速率一致。

**附带发现**：板载只有一颗麦克风，立体声采集时**右声道恒为数字 0**。要真实音频内容，
按单声道采集（`recordraw <file> 1 16 16000 0`）。

#### (3) 顺带补上的两处完成语义

同一轮还补了两件事，否则播放器仍然收不了尾：①驱动认出上层标的 `AUDIO_APB_FINAL`，
把尾巴真的送出去（再等两个半区）后交 `AUDIO_CALLBACK_COMPLETE`；②`stop()` 对放音方向
把 `pendq` 里未服务的缓冲以 `nbytes = 0` 交还上层（采集方向仍保持"丢弃"，原因见 6.2）。

**经验**：**"日志正常"不等于"硬件被配过"**。这两个缺陷都不产生任何错误码，唯一的破绽是
**"字节数/时序与标称值对不上"**——所以驱动验证必须做数值对账（落盘字节 ÷ 时长、中断次数 ×
半区大小），而不是只看日志有没有报错。这一条与 7.7 里"不报错不等于成功"是同一类教训，
只是这次藏在 HAL 的"配置结构体 vs 寄存器"之间。

> **一个值得记录的工程教训**：本次曾出现"提交仓里 `.c` 与 `.h` 不匹配、评审 clone
> 后必然编译失败"的情况——原因是本地同步脚本只同步了 `.c` 而漏了 `.h`，而工作树里
> 恰是正确版本，所以本地一直编得过。**教训：交付前必须校验"仓 vs 工作树"逐字节一致，
> 而不是"我本地能编过"。** 已补上自动化校验脚本。

### 7.9 离线语音播报与在线 TTS（2026-09-20）

**定位变了**：作品要做"断网也能用的本地协同 + 语音播报"，播报就不能只有云一条路。
于是分成两条，各司其职：

| 路径 | 触发 | 依赖 | 状态（2026-09-20） |
|---|---|---|---|
| **离线播报** | `clip <name>`（8 条固定播报） | 无——音频随固件烧进 romfs `/etc/clips/` | ✅ 真机验证：`clip light_on` 播 40,960 字节（1.3 s）、`overruns=0`，全程无网络 |
| **在线 TTS（任意文本）** | `say <任意文本>` | 板子能上网 + LLM key | ✅ 通路验证到网络层：请求构造/回包解析/重采样/播放全过，只差网络 |
| **自动播报** | `say_auto on` 后 `ask` 的回复会被念出来 | 同上 | 同上（代码就绪） |

**为什么离线用预合成而不是板端合成**：离线合成任意中文需要本地语音引擎（音节拼接或
小神经 TTS），体积与工期都不是这次能覆盖的。固定播报预合成是**零依赖**的，任意文本仍走
在线 TTS——这是有意分工，不是偷懒，也不是"假装能离线说任意话"。

**在线 TTS 的实现要点**（`packages/ai_agent/src/voice/mimo_tts.c`）：

- MiMo 平台的 TTS **不在 `/v1/audio/speech`**（那四个候选路径全是 404），而在
  `/v1/chat/completions`：`{"model":"mimo-v2.5-tts","messages":[{"role":"assistant",
  "content":"<要念的字>"}],"audio":{"format":"wav","voice":"冰糖"}}`，音频以 **base64
  放在 `choices[0].message.audio.data`**。走文本 JSON 意味着板子现成的 TLS 客户端
  （`vela_tls.c`）直接可用，不需要二进制响应支持。
- 回包是 **24 kHz 单声道 16bit WAV**，本板时钟表只有 16k/48k，故在板端做 3:2
  重采样（取一个样点 + 相邻两点取平均）落到 16 kHz，与 `voice_tts_ops_t` 约定的输出一致。
  18 字一句话：回包约 266 KB、云端合成 2.2 s。

**播放后端**：`/dev/audio0` 直接用 NuttX audio ioctl（`AUDIOIOC_CONFIGURE / GETBUFFERINFO /
ALLOCBUFFER / ENQUEUEBUFFER / REGISTERMQ / START`），实现照 `nxplayer` 抄。
`audio_playback.c` 按 `dev_path` 前缀分流：`/dev/...` 走设备节点，其余仍走 media_player
（上游路径原样保留）。启动时机有讲究：**先入队两个缓冲再 START**，否则下层会先空跑半区。

**顺带修掉的驱动缺陷（假 overrun）**：每条放音流收尾都报 `overruns=2`——因为
`AUDIO_APB_FINAL` 之后排水的那两次半区服务也会去 `pendq` 取数据，取不到就计数，而那时
数据早已全部在暂存区里。修法：排水期间不再调 `sf32lb52_serve_half()`。修完 `clip`/`beep`
都是 `overruns=0`，这个计数器才重新可用作"上层供桶是否及时"的指标。

**在线通路仍缺一环：板子没有上网手段**。今天板子上网只有两条路——原生 USB（第二根
USB-C 线，PPP 到 PC）或 UART2 + USB-TTL / ESP32-S3 网关，**都需要物理接线**；接线到位前
`say` 会停在 `net_connect ... ret=0x52`（无路由）。**离线播报不受此影响**。

> ⚠️ **实测发现：控制台里敲 `restart` 会把控制台弄死**，与已知的 `quit` 同类
> （回车、`help`、`ifconfig` 全无回显，软件侧救不回来）。恢复只有物理 Reset 或重新烧录
> （sftool 会让芯片复位，实测重烧后控制台即恢复）。**演示时不要敲 `quit`，也不要敲 `restart`。**

### 7.10 屏幕点亮：让"说出来的话"同时出现在屏上（2026-09-20）

接上屏后启动日志**不再出现** `ft6146_touch_initialize failed: -5`（未接屏时每次都有），
并且 `lv_nuttx_lcd_create: lcd /dev/lcd0 open success`、`touchscreen /dev/input0 open success,
maxpoint 1` —— 面板与触摸都被认到。面板是 **CO5300 驱动的 AMOLED**，本板配置
**390×450**（`CONFIG_LCD_HOR_RES_MAX=390` / `VER_RES_MAX=450`）。

**为什么没直接用上游那套聊天气泡 UI**（`packages/ai_agent/src/ui/lvgl_ui_channel.c`）：
三处不匹配，都是量过之后才下的结论。

| 维度 | 上游假设 | 本板实际 |
|---|---|---|
| 几何 | 466×466 圆形表盘（`LVGL_UI_SCREEN_W/H 466`） | 390×450 方形 |
| 中文字体 | Vela 字体服务 `vg_font_create("MiSans-Medium")` 或 FreeType | 都没有（`CONFIG_LV_USE_FREETYPE` 未开） |
| LVGL 归属 | 系统（miwear）已初始化 LVGL 并持有显示 | 裸开发板上没人初始化 |

**做法**：新增 `src/ui/agent_screen.c`，照 `apps/examples/lvgldemo` 的方式自己拉 LVGL
（`lv_init` → `lv_nuttx_dsc_init`(fb=/dev/lcd0, input=/dev/input0) → `lv_nuttx_init`），
建一块标签屏，字体用 LVGL 自带的 **`lv_font_simsun_16_cjk`**（约 2500 常用字，不需要
FreeType、不需要外挂字体文件），`agent_main` 在 CLI 通道打印回答时同步上屏，`say <文本>`
也上屏——"看的"和"听的"是同一句话。

**两个踩过的坑**（都属于"编译通过、上板才知道"）：

1. **屏幕设备是异步注册的**。第一次实现把 LVGL 初始化放在 agent 启动流程里（P4，+275 ms），
   日志报 `[screen] no display on /dev/lcd0`（`rc=-19`）——因为 LCD 驱动由板级 worker
   任务稍后才注册设备节点。改成**独立线程先等设备出现（最多 20 s）再初始化**，同时所有
   LVGL 调用都收在那一个线程里（LVGL 非线程安全），文本经待更新缓冲区交接。
2. **defconfig 改字体必须清构建目录重编**。加了 `CONFIG_LV_FONT_SIMSUN_16_CJK=y` 后增量
   构建仍打印 `[screen] CJK font not built in: Chinese will not render`——与 7.8 开
   `nxplayer` 是同一个坑（incremental build 不重读 defconfig）。删掉 `out/<config>` 重来才生效。
3. 另外 LVGL 的 NuttX 移植会检查栈大小（`check_stack_size`），8 KB 被判太小，
   任务栈提到 16 KB 才安静。

**当前状态**：屏与触摸可用、agent 文本可上屏（内置中文字体已生效）。要在屏上显示
**带网络的实时回答**（如天气），仍需要板子有上网手段——见 7.9 末段。


