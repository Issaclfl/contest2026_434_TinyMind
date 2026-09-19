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
    ESP_LOGI(TAG, "model %u B, tokenizer %u B (%s)",
             (unsigned)s_bundle.model_size, (unsigned)s_bundle.tokenizer_size,
             s_bundle.mapped ? "mapped from flash" : "copied to PSRAM");

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

    /* The format is only known once the engine has the pack header in hand --
     * asking for it before init would print the default (fp32) no matter what
     * the pack says. */
    const llm_config_t *cfg = llm_engine_config();
    ESP_LOGI(TAG, "model: %d layers, dim %d, hidden %d, %d heads (%d kv), vocab %d, seq_len %d, weights %s",
             cfg->n_layers, cfg->dim, cfg->hidden_dim, cfg->n_heads, cfg->n_kv_heads,
             cfg->vocab_size, cfg->seq_len, llm_engine_format_name());
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

void espllm_utf8_sanitize(char *s)
{
    if (s == NULL) {
        return;
    }
    unsigned char *p = (unsigned char *)s;
    unsigned char *o = (unsigned char *)s;
    while (*p != '\0') {
        int n = 0;
        if (*p < 0x80) {
            n = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            n = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            n = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            n = 4;
        }
        /* n == 0：孤立的延续字节（0x80..0xBF）或非法首字节，丢掉。 */
        bool good = n > 0;
        for (int i = 1; good && i < n; i++) {
            good = (p[i] & 0xC0) == 0x80;   /* p[i] 是 NUL 时这里自然为假 */
        }
        if (!good) {
            p++;
            continue;
        }

        /* 3 字节且落在 ED A0..BF：这是**代理对**，不是合法 UTF-8。
         * 实测云端就是这么发 emoji 的（🐔 发成 ED A0 BD ED B0 94，CESU-8），
         * 而浏览器那侧的 json 解析会直接报 "invalid continuation byte"。
         * 成对的还原成 4 字节 UTF-8（emoji 能留住），落单的丢掉。 */
        if (n == 3 && (p[0] & 0xF0) == 0xE0 && p[0] == 0xED && p[1] >= 0xA0 && p[1] <= 0xBF) {
            unsigned cp1 = ((unsigned)(p[0] & 0x0F) << 12) |
                           ((unsigned)(p[1] & 0x3F) << 6) | (unsigned)(p[2] & 0x3F);
            if (cp1 >= 0xD800 && cp1 <= 0xDBFF &&
                p[3] == 0xED && p[4] >= 0xB0 && p[4] <= 0xBF && (p[5] & 0xC0) == 0x80) {
                unsigned cp2 = ((unsigned)(p[3] & 0x0F) << 12) |
                               ((unsigned)(p[4] & 0x3F) << 6) | (unsigned)(p[5] & 0x3F);
                if (cp2 >= 0xDC00 && cp2 <= 0xDFFF) {
                    unsigned cp = 0x10000u + ((cp1 - 0xD800u) << 10) + (cp2 - 0xDC00u);
                    *o++ = (unsigned char)(0xF0 | (cp >> 18));
                    *o++ = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                    *o++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                    *o++ = (unsigned char)(0x80 | (cp & 0x3F));
                    p += 6;
                    continue;
                }
            }
            p += 3;                        /* 落单的代理：丢掉 */
            continue;
        }

        for (int i = 0; i < n; i++) {
            *o++ = p[i];
        }
        p += n;
    }
    *o = '\0';
}
