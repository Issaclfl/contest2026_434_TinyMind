/*
 * 命令模型 + 动作执行：本地小模型把英文口令翻成闭集动作 JSON，由这里执行。
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化动作侧（板载 RGB 灯）。幂等，失败只警告不影响其它功能。 */
void cmd_exec_init(void);

/* 跑一次命令模型并执行它给出的动作。
 * text     用户口令（英文），例如 "turn on the red light"
 * out/out_size 人类可读的结果（动作 JSON 或错误说明）
 * 返回 0 表示模型给出了一个合法动作并且已执行。 */
int cmd_exec_run(const char *text, char *out, size_t out_size);

/* 直接执行一个闭集里的 led 动作（中文分类器那条路走这里，不经过生成式小模型）。
 * color  red/green/blue/white/yellow/rgb/off
 * effect ""/blink/breath        times 闪烁次数（>=1）
 * out    机器可读的 JSON 结果；zh 说给用户听的中文（板子念的是这句）
 * 返回 0 表示动作真的做成了。 */
int cmd_exec_led(const char *color, const char *effect, int times,
    char *out, size_t out_size, char *zh, size_t zh_size);

#ifdef __cplusplus
}
#endif
