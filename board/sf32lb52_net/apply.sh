#!/bin/bash
# ##############################################################################
# SF32LB52 网络承载（PPP over UART2）—— 一键落盘脚本
#
# 作用：把 3 个生产树文件的改动打到树里，使板子具备一条能承载 IP 的链路。
#       板级 defconfig 由 board/sf32lb52_audio/ai_agent_port/ 负责，不在本脚本内。
#
# 用法（在 openvela 工作区根目录，或任意位置）：
#   bash contest2026_434_TinyMind/board/sf32lb52_net/apply.sh
# ##############################################################################

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "$HERE/../../.." && pwd)"
PATCHES="$HERE/patches"

echo "openvela 工作区 : $WORK"
echo

if [ ! -d "$WORK/apps" ] || [ ! -d "$WORK/vendor/sifli" ]; then
  echo "错误：找不到 apps / vendor/sifli，请确认已在 repo sync 后的工作区中运行。" >&2
  exit 1
fi

apply_one() {
  local repo="$1" patch="$2" label="$3"

  if git -C "$WORK/$repo" apply --check "$patch" 2>/dev/null; then
    git -C "$WORK/$repo" apply "$patch"
    echo "  [applied] $label"
  elif git -C "$WORK/$repo" apply --reverse --check "$patch" 2>/dev/null; then
    echo "  [skip]    $label  （已经打过了）"
  else
    echo "错误：$label 打不上，$repo 的工作树可能已被别的改动弄脏：" >&2
    git -C "$WORK/$repo" apply --check "$patch" >&2 || true
    exit 1
  fi
}

apply_one "apps" \
  "$PATCHES/0001-pppd-example-direct-serial-link.patch" \
  "apps/examples/pppd/pppd_main.c   (支持直连串口：tty/波特率参数，跳过 modem 拨号)"

apply_one "apps" \
  "$PATCHES/0002-pppd-rx-buffer-for-1500-mtu.patch" \
  "apps/netutils/pppd/ppp_conf.h    (收缓冲 1024 → 2048，容纳 1500 MTU 的帧)"

apply_one "vendor/sifli" \
  "$PATCHES/0003-sf32lb52-declare-serial-termios.patch" \
  "vendor/sifli/chips/sf32lb52/Kconfig (声明 ARCH_HAVE_SERIAL_TERMIOS)"

echo
echo "完成。校验："
grep -q "PPPD_DEFAULT_BAUD" "$WORK/apps/examples/pppd/pppd_main.c" \
  && echo "  [ok] pppd 例程支持直连串口"
grep -q "define PPP_RX_BUFFER_SIZE      2048" "$WORK/apps/netutils/pppd/ppp_conf.h" \
  && echo "  [ok] PPP 收缓冲已放大"
grep -q "select ARCH_HAVE_SERIAL_TERMIOS" "$WORK/vendor/sifli/chips/sf32lb52/Kconfig" \
  && echo "  [ok] 串口 termios 能力已声明"

echo
echo "下一步："
echo "  bash $WORK/contest2026_434_TinyMind/board/sf32lb52_audio/apply.sh   # 音频部分（若还没打）"
echo "  cp $WORK/contest2026_434_TinyMind/board/sf32lb52_audio/ai_agent_port/configs/ai_agent/defconfig \\"
echo "     $WORK/vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/ai_agent/defconfig"
echo "  cd $WORK && source build/envsetup.sh"
echo "  lunch vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd/configs/ai_agent"
echo "  m -j8"
echo
echo "提示：改过 defconfig 后必须删掉 out/ 下对应的构建目录再 lunch，"
echo "      否则增量构建会复用旧的 .config，新符号不会生效。"
