/*
 * espllm -- run a llama2.c checkpoint on an ESP32-S3 and serve it over HTTP.
 *
 * The engine is generated from upstream llama2.c by tools/port_llama2.py; see
 * espllm_engine.h for the parts of it this component wraps.  This header is
 * the API the applications use.
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* Mirrors the 7-int header of a llama2.c checkpoint.  The engine validates the
 * values and the component static-asserts the size, so drift shows up at build
 * time or as a loud log line rather than as nonsense output. */
typedef struct {
    int dim;
    int hidden_dim;
    int n_layers;
    int n_heads;
    int n_kv_heads;
    int vocab_size;
    int seq_len;
} espllm_config_t;

/* Loads the packed model (see tools/fetch_model.py) from a data partition and
 * builds the engine.  Call once, before anything else here. */
esp_err_t espllm_init(const char *partition_name, uint8_t subtype, bool prefer_psram);

bool espllm_ready(void);
const espllm_config_t *espllm_config(void);

/* Generates up to max_new_tokens after prompt.  Serialized internally, so it
 * is safe to call from several tasks -- one generation runs at a time. */
esp_err_t espllm_generate(const char *prompt, int max_new_tokens);

const char *espllm_text(void);
size_t espllm_text_len(void);
bool espllm_text_truncated(void);
double espllm_tok_per_sec(void);

/* The weight format the loaded pack turned out to be ("fp32", "int8 quantized",
 * "int4 quantized").  Useful in a log line or a status page; the value comes
 * from the pack header, not from a build-time setting. */
const char *espllm_weight_format(void);

/* Replaces the temperature the model was initialized with.  A client that
 * wants a reproducible answer sends temperature 0 (greedy); anything applied
 * here lasts until the next call. */
void espllm_set_temperature(float temperature);

/* ---- optional hooks: request routing and the status page -------------------
 *
 * The router gets every chat-completions body before local generation starts.
 * It returns ESP_OK with *response pointing at a NUL-terminated JSON document
 * (heap, freed here) to serve that verbatim as a 200, or any other result to
 * fall through to the local model.  Unset -- the default -- means every
 * request is answered locally. */
typedef esp_err_t (*espllm_router_fn)(const char *body, size_t body_len, char **response);

/* The status provider appends its own lines to GET / (and only there), after
 * the model's own status.  Returns the number of bytes written.  Unset means
 * the page shows only the model's own state. */
typedef int (*espllm_status_fn)(char *out, size_t out_size);

void espllm_set_router(espllm_router_fn router);
void espllm_set_status_provider(espllm_status_fn provider);

/* ---- 语音入口（组件不关心音频从哪来、送去哪）---------------------------------
 *
 * POST /voice 会把请求体原样交给 handler（WAV 或裸 PCM，最大 512 KB），
 * handler 返回一段 JSON 字符串作为响应 —— 和 router 是同一个约定。
 *
 * GET /talk 直接把 page 当网页发出去。它不是状态页，而是给手机/电脑用的
 * "按住说话"页面：录一段音、POST 给 /voice、把结果打出来。page 为 NULL 时
 * 这个路由会说"没配置语音"。 */
typedef esp_err_t (*espllm_voice_fn)(const char *audio, size_t audio_len, char **response);

void espllm_set_voice(espllm_voice_fn handler, const char *page);

/* Copies the first JSON string value stored under `key` into out (decoded,
 * NUL-terminated).  False when there is no such key or it holds something
 * other than a string.  Enough for routing decisions; not a JSON parser. */
bool espllm_json_field(const char *json, const char *key, char *out, size_t out_size);

/* Same, but the **last** match.  The board's agent sends its system prompt, its
 * tool catalogue and the user's turn in one body, and the user's turn is the
 * last "content" -- a first-match search would read the system prompt instead.
 * The command route needs exactly that one. */
bool espllm_json_field_last(const char *json, const char *key, char *out, size_t out_size);

/* 把不合法的 UTF-8 序列删掉，原地整理（长度只会变短）。
 *
 * 为什么需要：从云端拿回来的文本**可能在 max_tokens 处被截断，正好切在一个多字节
 * 字符中间**（实测：一句英文回答后面的 emoji 被砍成半个），那样拼进 JSON 就是非法
 * UTF-8，浏览器直接解不出来（python json.load 报 "invalid continuation byte"）。
 * 与其在好几个地方各自转义，不如在拿回来的地方统一清一遍。 */
void espllm_utf8_sanitize(char *s);

/* OpenAI-compatible endpoints on `port`:
 *     GET  /                     dashboard (browser friendly, auto-refresh)
 *     GET  /status               the same information as plain text
 *     GET  /v1/models            the one model this device serves
 *     POST /v1/chat/completions  {"messages":[{"role":"user","content":...}]}
 * Reachable on every interface that has an address, so the board can point its
 * agent's set_llm at http://10.0.0.1/v1 once the PPP link is up. */
esp_err_t espllm_http_start(uint16_t port);

/* 同一组路由再开一个 TLS 监听口，只是套了 TLS。
 *
 * 为什么需要：**浏览器只在安全上下文里给麦克风**。http://<ip>/talk 送出的页面里
 * navigator.mediaDevices 直接是 undefined（桌面 Chrome 与手机浏览器一致，实测
 * isSecureContext=false → 按钮点了没反应，控制台只有一句 "Cannot read properties
 * of undefined"）。板上 Agent 走的是明文那条腿（http://10.0.0.1/v1），所以两个口
 * 并存：明文给板子，TLS 给浏览器。
 *
 * cert/key 是 PEM 文本（app 侧嵌入）。长度按 _binary_..._end - _start 传，含结尾
 * 的 NUL —— esp_https_server 自己的惯例。 */
esp_err_t espllm_http_start_secure(uint16_t port, const uint8_t *cert, size_t cert_len,
                                   const uint8_t *key, size_t key_len);
