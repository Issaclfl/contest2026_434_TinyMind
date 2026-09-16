#!/bin/bash
# 校验：提交仓的 4 个驱动文件必须与工作树逐字节一致（否则评审编译会失败）
REPO=/home/lawson/openvela/contest2026_434_TinyMind/board/sf32lb52_audio
LCD=/home/lawson/openvela/vendor/sifli/boards/sf32lb52/sf32lb52_devkit_lcd

echo "===== 仓 vs 工作树 逐字节比对 ====="
ok=1
for pair in "src/bsp_audio.c" "src/bsp_audio_hw.c" "include/bsp_audio.h" "include/bsp_audio_hw.h"; do
  a=$(md5sum "$REPO/$pair" | cut -d' ' -f1)
  b=$(md5sum "$LCD/$pair" | cut -d' ' -f1)
  if [ "$a" = "$b" ]; then
    echo "  [一致] $pair   ${a:0:12}"
  else
    echo "  [不一致] $pair  仓=${a:0:12} 树=${b:0:12}"
    ok=0
  fi
done

echo
echo "===== 头文件里关键宏/类型是否齐备（缺一个就编不过）====="
H="$REPO/include/bsp_audio_hw.h"
for sym in SF32LB52_AUDIO_STAGE_BYTES SF32LB52_AUDIO_STAGE_HALVES \
           SF32LB52_AUDIO_DMA_ALIGN sf32lb52_audio_stage_cb_t \
           sf32lb52_audio_hw_set_stage_callback sf32lb52_audio_hw_stage_pointer; do
  n=$(grep -c "$sym" "$H" || true)
  [ "$n" -ge 1 ] && echo "  [ok]   $sym" || { echo "  [MISS] $sym"; ok=0; }
done

echo
echo "===== 仓内 .c 用到的头文件符号是否都在头文件里 ====="
for sym in SF32LB52_AUDIO_STAGE_BYTES SF32LB52_AUDIO_STAGE_HALVES sf32lb52_audio_stage_cb_t; do
  used=$(cat "$REPO/src/bsp_audio.c" "$REPO/src/bsp_audio_hw.c" | grep -c "$sym" || true)
  decl=$(grep -c "$sym" "$H" || true)
  echo "  $sym: .c 用 $used 次 / .h 定义 $decl 次"
done

echo
if [ "$ok" = "1" ]; then
  echo "★ 校验通过：仓内 4 个文件与工作树一致，且头文件符号齐备 → 评审 clone 后可编译"
else
  echo "!! 校验未通过，需修正"
fi
