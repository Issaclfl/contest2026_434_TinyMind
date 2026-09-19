# SF32LB52-DevKit-LCD：openvela AI Agent（openvelaClaw）首次移植

## 这是什么

把 openvela 官方的端侧 AI Agent 引擎（`packages/ai_agent`，即官网所称的
**openvelaClaw**）移植到思澈 **SF32LB52-DevKit-LCD**（Cortex-M33）上，并编译链接通过。

官方 `packages/ai_agent/defconfigs/` 原本只支持三块板：

| 已支持 | 架构 |
|---|---|
| goldfish-arm64-v8a-ap | arm64（QEMU 模拟器） |
| esp32s3-eye | xtensa 32 位（ESP32-S3） |
| gemini-s1 | armv7-a（全志 R528） |

**SF32LB52 是第四块，也是 armv8-m 上的第一块。**

## 实测结果（真机固件，非估算）

构建产物 `out/sifli_sf32lb52_devkit_lcd_ai_agent/nuttx.bin`：

| 阶段 | 大小 | md5 |
|---|---|---|
| 首次移植（仅 Agent + 音频驱动） | 2,185,240 B | `af4e83e00cff` |
| **当前随包固件**（+ 网络栈 + `nxplayer` + 放音/采集通道修复） | **2,249,088 B** | **`4fa0eade7844f22a55f561bc7723aa91`** |

当前固件的资源占用：

| 区域 | 占用 | 容量 | 占比 |
|---|---|---|---|
| flash | 2,249,088 B | 16 MB | 13.40% |
| sram | ≈153 KB | 512 KB | ≈29.3% |
| psram | 0 B | 8 MB | 0%（尚未启用为堆区） |

相比仅含音频驱动的基线（1,451,508 B），Agent 增加约 **778 KB**。

Agent 已注册为 NSH 内置命令，栈 32 KB：

```
{ "ai_agent", 100, 32768, ai_agent_main }
```

## 落地需要三件事（缺一不可）

### 1. 补回 `packages/CMakeLists.txt`（前置缺陷，影响所有参赛队）

`apps/CMakeLists.txt:78` 是 `if(EXISTS packages/CMakeLists.txt)`，而
基础 manifest `openvela.xml:206` 的 `<project path="packages" name="packages"/>`
在当前工程里**没有同步下来**，于是该文件缺失，**整棵 `packages` 树从不参与构建**——
组委会自己发的 `app/hello_app` 骨架（映射到 `packages/demos/`）同样编不进去。

从 Gitee 的 `open-vela/packages`（`dev-ai-contest-2026` 分支）取回官方文件即可。
它只有两行实质内容，且同时解决 CMake 与 Kconfig 两道闸门
（`nuttx_generate_kconfig` 说明该树的 Kconfig 是代码生成的）：

```cmake
nuttx_add_subdirectory()
nuttx_generate_kconfig(MENUDESC "Packages")
```

副本见同目录 `packages-CMakeLists.txt`。

### 2. 板级 defconfig

见 `configs/ai_agent/defconfig`（本目录内为副本，实际路径
`vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/ai_agent/defconfig`）。

要点：

- **网络全栈**：本板无 WiFi，网络栈原本整个关闭（`# CONFIG_NET is not set`）。
  Agent 需要 TCP/UDP/DNS/ICMP/netlib/cJSON，需逐项开启。
  承载 IP 的传输层（PPP over UART）是后续步骤，本阶段只求编译链接通过。
- **`CONFIG_NETDEV_IFINDEX=y`**：`getifaddrs()`/`freeifaddrs()` 由
  `nuttx/libs/libc/net/Make.defs:37` 用该开关守卫，被 `network_manager.c` 使用。
  不开启会在链接期报 `undefined reference to 'getifaddrs'`。
- **重通道全关**：`AI_AGENT_FEISHU` / `WEIXIN` / `MQTT` / `NODE` / `MCP`
  （照 ESP32-S3 那份参考配置的裁剪法，它们合计约 213 KB RAM）。
- **不要覆盖板子自己的栈配置**：defconfig 只应"追加基线里没有的符号"。
  首次尝试时重复定义了 `DEFAULT_TASK_STACKSIZE`（把板子的 16096 降成 4096）
  等 5 个符号，Kconfig 报 "set more than once"，且栈变小会在运行时变成极难查的崩溃。

### 3. 两处工具链兼容性修复（`fix_sf32lb52.sh` 要做的事）

厂商测试用的工具链（xtensa / arm64）比我们的 arm-none-eabi-gcc 13.2.1 宽松，
以下两处在 `-Werror` 下会直接编译失败：

| 文件 | 问题 | 修法 |
|---|---|---|
| `src/core/agent_loop.c:665` | `snprintf(reply, 512, "...%s", remind_msg)`，而 `remind_msg` 最长 511 字节，前缀约 39 字节 → `-Werror=format-truncation` | 源串改为 `%.440s`，保留 512 字节的 reply 契约 |
| `src/tools/skill_loader.c:449` | `%08x` 配 `uint32_t`。**arm-none-eabi 上 `uint32_t` 是 `unsigned long`，不是 `unsigned int`** → `-Werror=format=` | 加 `(unsigned)` 转换（在 arm64/xtensa 上是空操作，故对官方平台同样正确） |

diff 见 `../patches/0005-ai_agent-fix_sf32lb52.patch`。
按官方仓库既有约定，这类"无法用 defconfig 表达"的改动应做成
`fix_<device>.sh`（仓库里已有 `fix_esp32s3.sh`、`fix_gemini_s1.sh` 两个先例）。

## 复现步骤

```bash
cd <openvela 工程根目录>

# 1. 补回 packages 构建闸门（若工程里还没有）
cp <本目录>/packages-CMakeLists.txt packages/CMakeLists.txt

# 2. 落板级配置
cp <本目录>/configs/ai_agent/defconfig \
   vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/ai_agent/defconfig

# 3. 打工具链兼容性补丁
git -C packages/ai_agent apply <本目录>/../patches/0005-ai_agent-fix_sf32lb52.patch

# 4. 构建
source build/envsetup.sh
lunch vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/ai_agent
m -j6
```

> ⚠️ 改过 `defconfig` 后必须让 `.config` 重新生成，否则增量构建会复用旧配置
> （`rm -rf out/sifli_sf32lb52_devkit_lcd_ai_agent` 后再 `lunch`）。这是本项目
> 适配难点里记录的第三条同类陷阱。

## 尚未完成

- **IP 承载**：板子尚无 IP，Agent 无法访问 LLM 端点。下一步是 PPP over UART
  + PC 侧 pppd 网关。
- **运行期内存**：PSRAM（8 MB，当前 0% 使用）应作为第二堆区启用，
  官方称 Agent 最小配置约需 248 KB RAM。
- **语音通道**：`AGENT_AUDIO_CAPTURE_DEV` 默认走 Vela 媒体框架
  （`media_recorder`），而非直接调 NuttX 音频 ioctl，这一环尚未打通。
