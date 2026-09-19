/*
 * Cloud LLM route: the gateway decides per request whether the question goes
 * to the cloud model or to the local one.
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the cloud route with the local model's HTTP server.
 *
 * From here on the model endpoint doubles as a routing point: a request whose
 * model name says "local" (or names the local model) is answered locally, and
 * every other request tries the cloud first, falling back to the local model
 * when the uplink is gone or the cloud call fails.  The board never sees any
 * of this -- for it the endpoint stays one address.
 *
 * Safe to call when no cloud is configured: the route then counts requests
 * and answers locally, which is also the whole story when the key is empty.
 */
void net_cloud_start(void);

/** True once an API key is configured (never logs or serves the key itself). */
bool net_cloud_configured(void);

/** 网关自己的状态行（路由统计、上行、板子会话、RSSI），与 GET /status 追加在
 * 模型信息后的那段同源。返回写入字节数。命令模型的 status 动作用它作答。 */
int net_cloud_status(char *out, size_t out_size);

/**
 * 把一句话当成一个 user 消息问云端模型，回答正文写进 out。返回 0 成功，-1 失败
 * （out 里写的是给人看的失败原因）。
 *
 * 命令模型的 `ask` 动作用它：本地 0.26M 的小模型只负责判断"这题该去问云端"，
 * 原话由这里原样转出去（复述文本正是小模型最不擅长的活，所以不让它复述）。
 * 没有上行或没有 key 时如实返回失败，不编一个答案出来。
 */
int net_cloud_ask_text(const char *text, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
