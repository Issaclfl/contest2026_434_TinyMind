/*
 * Getting the model into the address space.
 *
 * The checkpoint and its tokenizer live together in one data partition (see the
 * project's partitions.csv), as a small header followed by the two files:
 *
 *     offset 0   u32 magic ('LLM1')
 *     offset 4   u32 model_size
 *     offset 8   u32 tokenizer_size
 *     offset 12  u32 weight format: 0 = fp32 (fetch_model.py), 1 = int8, 2 = int4
 *     offset 16  model bytes, then tokenizer bytes
 *
 * The format field sits in what used to be four reserved bytes, so a pack made
 * before quantization existed already reads as format 0.  tools/quantize_model.py
 * writes 1 or 2; the value travels to the engine unchanged.
 *
 * tools/fetch_model.py writes that file; tools/flash_model.py puts it in the
 * partition.  Self-describing means the firmware never has to guess a length
 * from the partition size -- a partition is padded to the flash sector, and
 * copying that padding into PSRAM would waste megabytes.
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

#define LLM_BUNDLE_MAGIC 0x314D4C4Cu   /* 'LLM1' little-endian */

/* Values the pack header's format field can hold.  Keep in step with the
 * LLM_QFMT_* the generated engine uses (espllm_engine.c, written by
 * tools/port_llama2.py). */
#define LLM_BUNDLE_FMT_FP32 0u
#define LLM_BUNDLE_FMT_INT8 1u
#define LLM_BUNDLE_FMT_INT4 2u

typedef struct {
    const void *model;
    size_t model_size;
    const void *tokenizer;
    size_t tokenizer_size;
    uint32_t format;          /* weight format; see the header layout above */

    /* Ownership bookkeeping; the views above point either into the mapping or
     * into the PSRAM copy. */
    esp_partition_mmap_handle_t handle;
    bool mapped;
    void *owned;
} llm_bundle_t;

/* Finds the data partition, checks the header and resolves the two views.
 *
 * When the model fits in PSRAM with room to spare for the run state it is
 * copied there (weights are re-read for every token, and PSRAM beats the
 * flash cache); otherwise it stays memory-mapped in place. */
esp_err_t llm_bundle_open(const char *part_name, uint8_t subtype, bool prefer_psram,
                          llm_bundle_t *out);

void llm_bundle_close(llm_bundle_t *bundle);
