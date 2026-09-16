#!/bin/bash
# ##############################################################################
# SF32LB52 音频驱动适配 —— 一键落盘脚本
#
# 作用：把 4 个必须改动的生产树文件覆盖到位，使驱动能进编译并注册 /dev/audio0。
#       4 个新增源文件由 manifest 的 <linkfile> 自动就位，本脚本会检查它们。
#
# 用法（在 openvela 工作区根目录，或任意位置）：
#   bash contest2026_434_TinyMind/board/sf32lb52_audio/apply.sh
# ##############################################################################

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/../../.." && pwd)"
BOARD="$WORK/vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd"

echo "openvela 工作区 : $WORK"
echo "SiFli 板级目录  : $BOARD"
echo

if [ ! -d "$BOARD" ]; then
  echo "错误：找不到 SiFli 板级目录。请确认已在 repo sync 后的工作区中运行。" >&2
  exit 1
fi

# ---------- 1. 新增源文件是否已由 linkfile 就位 ----------
MISSING=0
for f in src/bsp_audio.c src/bsp_audio_hw.c include/bsp_audio.h include/bsp_audio_hw.h; do
  if [ -e "$BOARD/$f" ]; then
    echo "  [ok]      $f"
  else
    # linkfile 未生效时兜底复制
    cp "$HERE/$f" "$BOARD/$f"
    echo "  [copied]  $f  (linkfile 未生效，已兜底复制)"
    MISSING=$((MISSING + 1))
  fi
done
echo

# ---------- 2. 覆盖 4 个改动的生产树文件 ----------
copy_over() {
  local src="$1" dst="$2" label="$3"
  if [ ! -f "$src" ]; then
    echo "错误：缺少 $src" >&2
    exit 1
  fi
  cp "$src" "$dst"
  echo "  [applied] $label"
}

copy_over "$HERE/board_files/src_CMakeLists.txt"    "$BOARD/src/CMakeLists.txt"                    "src/CMakeLists.txt          (SRCS 追加 bsp_audio.c bsp_audio_hw.c)"
copy_over "$HERE/board_files/drivers_CMakeLists.txt" "$WORK/vendor/sifli/boards/sf32lb52/drivers/CMakeLists.txt" "drivers/CMakeLists.txt      (厂商代码关闭 -Werror)"
copy_over "$HERE/board_files/defconfig"             "$BOARD/configs/nsh/defconfig"                 "configs/nsh/defconfig       (CONFIG_AUDIO=y)"
copy_over "$HERE/board_files/sifli_ap.c"            "$BOARD/src/sifli_ap.c"                        "src/sifli_ap.c              (注册 /dev/audio0)"

echo
echo "完成。校验："
grep -q "bsp_audio.c" "$BOARD/src/CMakeLists.txt"                     && echo "  [ok] 驱动已进 CMake SRCS"
grep -q "^CONFIG_AUDIO=y" "$BOARD/configs/nsh/defconfig"              && echo "  [ok] CONFIG_AUDIO=y"
grep -q "audio_register(\"/dev/audio0\"" "$BOARD/src/sifli_ap.c"      && echo "  [ok] /dev/audio0 注册代码就位"
echo
echo "下一步："
echo "  cd $(cd "$HERE/../../.." && pwd)"
echo "  source build/envsetup.sh"
echo "  lunch vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/nsh"
echo "  m -j8"
echo
echo "提示：若改过 defconfig 而配置未生效（grep CONFIG_SYSTEM_NXRECORDER out/.../.config"
echo "      仍显示 not set），先删除 out/sfili... 对应的构建目录再重新 lunch + m。"
