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

#ifdef __cplusplus
}
#endif
