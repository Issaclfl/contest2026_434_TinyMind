/*
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include "espllm_blob.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "espllm.model";

#define HEADER_BYTES 16u

/* Keep this much PSRAM for the run state: the KV cache of even a tiny model is
 * hundreds of KB (stories260K needs 640 KB), so a copy that leaves no room for
 * the caches is a losing trade. */
#define PSRAM_HEADROOM_BYTES (768 * 1024)

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t llm_bundle_open(const char *part_name, uint8_t subtype, bool prefer_psram,
                          llm_bundle_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)subtype, part_name);
    if (part == NULL) {
        ESP_LOGE(TAG, "no data partition \"%s\" (subtype 0x%02x) in the table", part_name, subtype);
        return ESP_ERR_NOT_FOUND;
    }

    /* Header first, before deciding how much memory this needs. */
    uint8_t header[HEADER_BYTES];
    esp_err_t err = esp_partition_read(part, 0, header, sizeof(header));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot read the header: %s", esp_err_to_name(err));
        return err;
    }
    uint32_t magic = read_u32(header);
    uint32_t model_size = read_u32(header + 4);
    uint32_t tok_size = read_u32(header + 8);
    if (magic != LLM_BUNDLE_MAGIC) {
        ESP_LOGE(TAG, "partition \"%s\" holds 0x%08x, not a packed model (magic 0x%08x).",
                 part_name, (unsigned)magic, (unsigned)LLM_BUNDLE_MAGIC);
        ESP_LOGE(TAG, "flash it: tools\\flash_model.bat COMx");
        return ESP_ERR_INVALID_STATE;
    }
    size_t need = HEADER_BYTES + model_size + tok_size;
    if (model_size == 0 || tok_size == 0 || need > part->size) {
        ESP_LOGE(TAG, "header says %u + %u bytes, which does not fit the %u B partition",
                 (unsigned)model_size, (unsigned)tok_size, (unsigned)part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    bool copied = false;
    if (prefer_psram) {
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (need + PSRAM_HEADROOM_BYTES <= free_psram) {
            void *copy = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
            if (copy != NULL) {
                err = esp_partition_read(part, 0, copy, need);
                if (err == ESP_OK) {
                    out->owned = copy;
                    out->mapped = false;
                    copied = true;
                    ESP_LOGI(TAG, "%u B model copied to PSRAM (%u B were free)",
                             (unsigned)need, (unsigned)free_psram);
                } else {
                    ESP_LOGW(TAG, "PSRAM read failed (%s); mapping instead", esp_err_to_name(err));
                    heap_caps_free(copy);
                }
            } else {
                ESP_LOGW(TAG, "no %u B of PSRAM; mapping instead", (unsigned)need);
            }
        } else {
            ESP_LOGI(TAG, "%u B does not fit in %u B of free PSRAM; mapping it",
                     (unsigned)need, (unsigned)free_psram);
        }
    }

    const char *base = NULL;
    if (copied) {
        base = (const char *)out->owned;
    } else {
        const void *mapped = NULL;
        err = esp_partition_mmap(part, 0, need, ESP_PARTITION_MMAP_DATA, &mapped, &out->handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mmap of %u B failed: %s", (unsigned)need, esp_err_to_name(err));
            return err;
        }
        out->mapped = true;
        base = (const char *)mapped;
        ESP_LOGI(TAG, "%u B model mapped from flash", (unsigned)need);
    }

    out->model = base + HEADER_BYTES;
    out->model_size = model_size;
    out->tokenizer = base + HEADER_BYTES + model_size;
    out->tokenizer_size = tok_size;
    return ESP_OK;
}

void llm_bundle_close(llm_bundle_t *bundle)
{
    if (bundle == NULL) {
        return;
    }
    if (bundle->owned != NULL) {
        heap_caps_free(bundle->owned);
    } else if (bundle->mapped) {
        esp_partition_munmap(bundle->handle);
    }
    memset(bundle, 0, sizeof(*bundle));
}
