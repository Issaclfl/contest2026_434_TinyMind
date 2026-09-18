/*
 * The generated llama2.c engine (espllm_engine.c), declared.
 *
 * That file is produced by tools/port_llama2.py from third_party/llama2.c/run.c
 * and has no ESP-IDF dependencies; espllm.c wraps it with the blob handling and
 * a mutex.  Nothing outside this component includes this header.
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

/* Mirrors the 7-int header at the front of a llama2.c checkpoint (see Config in
 * third_party/llama2.c/run.c).  The engine validates the values it finds, and
 * espllm.c static-asserts that this matches the public espllm_config_t, so a
 * drift shows up at build time rather than as nonsense output. */
typedef struct {
    int dim;
    int hidden_dim;
    int n_layers;
    int n_heads;
    int n_kv_heads;
    int vocab_size;
    int seq_len;
} llm_config_t;

/* The weight formats a pack can carry: the value at offset 12 of the pack
 * header, handed here unchanged (see espllm_blob.h).  Must match the LLM_QFMT_*
 * the generated engine defines. */
#define LLM_QFMT_FP32 0
#define LLM_QFMT_INT8 1
#define LLM_QFMT_INT4 2

/* Returns 0 on success, negative if the blobs do not look like a checkpoint /
 * tokenizer.  `format` selects the weight layout: 0 is the upstream fp32
 * checkpoint, 1/2 are the row-quantized int8/int4 layouts written by
 * tools/quantize_model.py.  Not thread-safe: serialize callers. */
int llm_engine_init(const void *model_blob, size_t model_size,
                    const void *tokenizer_blob, size_t tokenizer_size,
                    int format,
                    float temperature, float topp, unsigned long long seed);

int llm_engine_ready(void);
const llm_config_t *llm_engine_config(void);

/* The weight format the engine was built with, and its name for logs. */
int llm_engine_format(void);
const char *llm_engine_format_name(void);

/* Sampling knob; 0 means greedy, which is what makes two weight formats
 * comparable token by token. */
void llm_engine_set_temperature(float temperature);

/* Generates up to max_new_tokens after the prompt; the decoded text is then in
 * llm_engine_text().  The prompt is not echoed back. */
int llm_engine_generate(const char *prompt, int max_new_tokens);

const char *llm_engine_text(void);
size_t llm_engine_text_len(void);
int llm_engine_text_truncated(void);
double llm_engine_tok_per_sec(void);
