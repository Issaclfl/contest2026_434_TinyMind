/*
 * host_check -- 在 PC 上跑同一份 espllm_engine.c，用来在烧板之前对齐格式与误差。
 *
 * 为什么要它：量化格式是"打包器 + 引擎映射"两边各写一遍的东西，写错任何一边
 * 都会得到"能跑但答案变差"的结果——这正是最难看出来的那类错。把引擎源码原样
 * 编到 PC 上，对着同一份 assets/llm_q8.bin 跑同一段 prompt，再和 fp32 对比，
 * 就能在开工前把格式对齐；烧到板子上之后只需要确认数字一致、顺便量 tok/s。
 *
 * 编译（在 esp32s3_common/ 下）：
 *     gcc -O2 -o /tmp/host_check tools/host_check.c \
 *         components/espllm/espllm_engine.c -Icomponents/espllm/include -lm
 * 用法：
 *     /tmp/host_check assets/llm_q8.bin "Once upon a time" 200 0
 *         参数：打包文件、prompt、最多生成多少 token、temperature（0 = 贪心）
 *     不带参数时跑 assets/llm.bin 的默认例子。
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espllm_engine.h"

#define MAGIC 0x314D4C4Cu

static unsigned char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path);
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static unsigned read_u32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "assets/llm.bin";
    const char *prompt = argc > 2 ? argv[2] : "Once upon a time, there was a little robot";
    int max_new = argc > 3 ? atoi(argv[3]) : 200;
    float temperature = argc > 4 ? (float)atof(argv[4]) : 0.0f;

    size_t size = 0;
    unsigned char *pack = read_file(path, &size);
    if (pack == NULL) {
        return 1;
    }
    if (size < 16 || read_u32(pack) != MAGIC) {
        fprintf(stderr, "%s is not a packed model (magic %08x)\n", path, read_u32(pack));
        free(pack);
        return 1;
    }
    unsigned model_size = read_u32(pack + 4);
    unsigned tok_size = read_u32(pack + 8);
    unsigned format = read_u32(pack + 12);
    if (16 + (size_t)model_size + tok_size > size) {
        fprintf(stderr, "%s is truncated\n", path);
        free(pack);
        return 1;
    }

    int rc = llm_engine_init(pack + 16, model_size, pack + 16 + model_size, tok_size,
                             (int)format, temperature, 0.9f, 1337);
    if (rc != 0) {
        fprintf(stderr, "engine init failed: %d\n", rc);
        free(pack);
        return 1;
    }
    const llm_config_t *cfg = llm_engine_config();
    fprintf(stderr, "%s: format %u (%s), model %u B, tokenizer %u B\n",
            path, format, llm_engine_format_name(), model_size, tok_size);
    fprintf(stderr, "model: %d layers, dim %d, hidden %d, %d heads (%d kv), vocab %d, seq_len %d\n",
            cfg->n_layers, cfg->dim, cfg->hidden_dim, cfg->n_heads, cfg->n_kv_heads,
            cfg->vocab_size, cfg->seq_len);
    fprintf(stderr, "prompt: %s | temperature %.2f | max_new %d\n---\n", prompt,
            (double)temperature, max_new);

    if (llm_engine_generate(prompt, max_new) != 0) {
        fprintf(stderr, "generation failed\n");
        free(pack);
        return 1;
    }
    printf("%s", llm_engine_text());
    fprintf(stderr, "\n--- %u chars, %.1f tok/s (host)\n",
            (unsigned)llm_engine_text_len(), llm_engine_tok_per_sec());
    free(pack);
    return 0;
}
