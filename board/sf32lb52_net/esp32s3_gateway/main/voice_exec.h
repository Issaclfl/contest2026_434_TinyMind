/*
 * 语音入口：POST /voice 收音频 -> 云端转写 -> 本地命令模型 -> 执行动作。
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 把语音处理器注册到本地模型的 HTTP 服务上（POST /voice + GET /talk）。
 * 需要已经调用过 espllm_init/espllm_http_start。 */
void voice_start(void);

#ifdef __cplusplus
}
#endif
