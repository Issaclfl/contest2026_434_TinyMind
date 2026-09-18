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

/* Copies the first JSON string value stored under `key` into out (decoded,
 * NUL-terminated).  False when there is no such key or it holds something
 * other than a string.  Enough for routing decisions; not a JSON parser. */
bool espllm_json_field(const char *json, const char *key, char *out, size_t out_size);

/* OpenAI-compatible endpoints on `port`:
 *     GET  /                     dashboard (browser friendly, auto-refresh)
 *     GET  /status               the same information as plain text
 *     GET  /v1/models            the one model this device serves
 *     POST /v1/chat/completions  {"messages":[{"role":"user","content":...}]}
 * Reachable on every interface that has an address, so the board can point its
 * agent's set_llm at http://10.0.0.1/v1 once the PPP link is up. */
esp_err_t espllm_http_start(uint16_t port);
