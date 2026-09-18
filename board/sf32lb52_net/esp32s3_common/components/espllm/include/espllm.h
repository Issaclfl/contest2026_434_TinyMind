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

/* OpenAI-compatible endpoints on `port`:
 *     GET  /                     status text (browser friendly)
 *     GET  /v1/models            the one model this device serves
 *     POST /v1/chat/completions  {"messages":[{"role":"user","content":...}]}
 * Reachable on every interface that has an address, so the board can point its
 * agent's set_llm at http://10.0.0.1/v1 once the PPP link is up. */
esp_err_t espllm_http_start(uint16_t port);
