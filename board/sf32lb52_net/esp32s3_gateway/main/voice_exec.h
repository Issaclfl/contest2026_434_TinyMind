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

/* 把语音处理器和说话页面注册到本地模型的 HTTP 服务上（POST /voice + GET /talk）。
 * 需要已经调用过 espllm_init/espllm_http_start。 */
void voice_start(void);

/* 再把同一组路由开一份 TLS（CONFIG_GATEWAY_VOICE_TLS_PORT，默认 443）。
 *
 * 不是花活，是必需：**浏览器只在安全上下文里给麦克风**，http 页面里
 * navigator.mediaDevices 是 undefined，按住说话点了没反应（手机和桌面一致）。
 * 所以"按住说一句就控制"只能走这个口；证书是自签的，见 main/README 里那段说明。 */
void voice_https_start(void);

#ifdef __cplusplus
}
#endif
