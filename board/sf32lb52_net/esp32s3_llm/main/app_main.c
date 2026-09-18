/*
 * ESP32-S3 local model node -- network-free demo.
 *
 * Brings up nothing but the model and generates from a fixed prompt, so the
 * numbers on the console are inference and only inference.  The same component
 * backs the Wi-Fi gateway (../esp32s3_gateway), where the model is reachable
 * over HTTP; this project exists to show the inference on its own and to have a
 * testbed that cannot be confused by networking.
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espllm.h"

static const char *TAG = "llm-demo";

/* Matches this project's partitions.csv: a custom data subtype in the range the
 * partition table leaves to applications. */
#define LLM_PART_NAME    "llm"
#define LLM_PART_SUBTYPE 0x40

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "%s: internal free %u B, PSRAM free %u B", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void log_text(const char *text)
{
    /* One log line per line of output: keeps the console readable and avoids
     * losing the tail of a long generation to a line-length limit. */
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        ESP_LOGI(TAG, "%.*s", (int)len, p);
        p += len + (nl ? 1 : 0);
    }
}

static void llm_task(void *arg)
{
    (void)arg;

    log_heap("boot");
    esp_err_t err = espllm_init(LLM_PART_NAME, LLM_PART_SUBTYPE, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "model not usable: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "flash it first: tools\\model.bat COMx  (see tools\\fetch_model.py)");
        goto out;
    }

    for (int round = 0; round < CONFIG_LLM_ROUNDS; round++) {
        const char *prompt = CONFIG_LLM_PROMPT;
        ESP_LOGI(TAG, "--- round %d/%d, prompt: %s", round + 1, CONFIG_LLM_ROUNDS, prompt);
        int64_t t0 = esp_timer_get_time();
        err = espllm_generate(prompt, CONFIG_LLM_MAX_TOKENS);
        int64_t t1 = esp_timer_get_time();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "generation failed: %s", esp_err_to_name(err));
            break;
        }
        ESP_LOGI(TAG, "--- output (%u chars%s, %.1f s wall clock):",
                 (unsigned)espllm_text_len(),
                 espllm_text_truncated() ? ", TRUNCATED" : "",
                 (t1 - t0) / 1e6);
        log_text(espllm_text());
        ESP_LOGI(TAG, "--- %.2f tok/s", espllm_tok_per_sec());
        log_heap("after generation");
    }

out:
    ESP_LOGI(TAG, "done; the model stays loaded, this task just ends");
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 local model node (llama2.c), target %s", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "prompt and sampling come from Kconfig: menuconfig -> Local model");

    /* app_main's own stack is small and this task only waits, so inference runs
     * on a task with room for the engine's frames. */
    BaseType_t ok = xTaskCreatePinnedToCore(llm_task, "llm", 32768, NULL, 5, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "could not start the inference task");
    }
}
