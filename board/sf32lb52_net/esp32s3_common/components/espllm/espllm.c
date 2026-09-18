/*
 * espllm -- public entry points: load the model, serialize generation.
 *
 * The inference itself is espllm_engine.c (generated from llama2.c, no IDF
 * dependencies).  This file adds the two things an application needs on top:
 * getting the blobs out of flash, and making sure only one generation runs at
 * a time (the engine writes into fixed buffers and is not reentrant -- the
 * kconfig help says so, and a mutex is cheaper than a queue of engines).
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include "espllm.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "espllm_blob.h"
#include "espllm_engine.h"

static const char *TAG = "espllm";

/* The public config struct and the engine's have to describe the same 7 ints
 * or every log line about the model would be a lie. */
_Static_assert(sizeof(espllm_config_t) == sizeof(llm_config_t),
               "espllm_config_t must match the engine's llm_config_t");

static llm_bundle_t s_bundle;
static SemaphoreHandle_t s_lock;
static bool s_ready;

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "%s: internal free %u B, PSRAM free %u B", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

esp_err_t espllm_init(const char *partition_name, uint8_t subtype, bool prefer_psram)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "no memory for the generation mutex");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = llm_bundle_open(partition_name, subtype, prefer_psram, &s_bundle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "model not usable: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "model %u B, tokenizer %u B (%s), weights %s",
             (unsigned)s_bundle.model_size, (unsigned)s_bundle.tokenizer_size,
             s_bundle.mapped ? "mapped from flash" : "copied to PSRAM",
             llm_engine_format_name());

    log_heap("before engine init");
    int rc = llm_engine_init(s_bundle.model, s_bundle.model_size,
                             s_bundle.tokenizer, s_bundle.tokenizer_size,
                             (int)s_bundle.format,
                             CONFIG_ESPLM_TEMPERATURE / 100.0f,
                             CONFIG_ESPLM_TOPP / 100.0f,
                             CONFIG_ESPLM_SEED);
    if (rc != 0) {
        ESP_LOGE(TAG, "engine init failed: %d (is the partition a packed llama2.c model?)", rc);
        return ESP_FAIL;
    }

    const llm_config_t *cfg = llm_engine_config();
    ESP_LOGI(TAG, "model: %d layers, dim %d, hidden %d, %d heads (%d kv), vocab %d, seq_len %d",
             cfg->n_layers, cfg->dim, cfg->hidden_dim, cfg->n_heads, cfg->n_kv_heads,
             cfg->vocab_size, cfg->seq_len);
    ESP_LOGI(TAG, "sampling: temperature %.2f, top-p %.2f, seed %d",
             CONFIG_ESPLM_TEMPERATURE / 100.0f, CONFIG_ESPLM_TOPP / 100.0f, CONFIG_ESPLM_SEED);
    log_heap("after engine init");

    s_ready = true;
    return ESP_OK;
}

bool espllm_ready(void)
{
    return s_ready;
}

const espllm_config_t *espllm_config(void)
{
    return s_ready ? (const espllm_config_t *)llm_engine_config() : NULL;
}

esp_err_t espllm_generate(const char *prompt, int max_new_tokens)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (prompt == NULL || max_new_tokens <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Wait forever rather than fail: a caller that gives up mid-generation
     * would leave the engine's buffers in an unknown state, and the only
     * clients here are the board and a browser. */
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int rc = llm_engine_generate(prompt, max_new_tokens);
    xSemaphoreGive(s_lock);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

const char *espllm_text(void)
{
    return llm_engine_text();
}

size_t espllm_text_len(void)
{
    return llm_engine_text_len();
}

bool espllm_text_truncated(void)
{
    return llm_engine_text_truncated() != 0;
}

double espllm_tok_per_sec(void)
{
    return llm_engine_tok_per_sec();
}

const char *espllm_weight_format(void)
{
    return s_ready ? llm_engine_format_name() : "unknown";
}

void espllm_set_temperature(float temperature)
{
    if (!s_ready) {
        return;
    }
    llm_engine_set_temperature(temperature);
}
