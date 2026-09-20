/*
 * 本地意图分类器（推理侧）—— 实现。
 *
 * 特征必须与训练端逐字节一致（tools/train_intent_clf.py）：
 *   - 按**字符**（UTF-8 codepoint）而不是按字节滑窗取 1-3 gram
 *   - 每个 gram 做 zlib 兼容的 CRC-32，再 % 4096 得到特征下标
 *   - 同一个 gram 出现多次就累加计数（词袋）
 *   - 特征向量做 L2 归一化后与权重点积，取 argmax
 * 归一化与量化都保序：整矩阵乘正数不影响 argmax，所以 int8 权重不损失结果。
 *
 * 性能：一句中文只有十几到几十个**非零**特征，所以点积是稀疏的 —— 65 个类别
 * 各乘几十次，毫秒级，不需要任何向量化技巧。
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include "net_clf.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "clf_weights.h"

#define CLF_MAX_CP 256        /* 一句指令最多 256 个字符（远超实际） */
#define CLF_MAX_FEAT 512      /* 一句话产生的不同 gram 数上限 */

static uint32_t s_crc_table[256];
static bool s_crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        s_crc_table[i] = c;
    }
    s_crc_ready = true;
}

/* 与 zlib.crc32 一致（CRC-32/ISO-HDLC）。 */
static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    if (!s_crc_ready) {
        crc_init();
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c = s_crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

int net_clf_classes(void)
{
    return CLF_CLASSES;
}

int net_clf_dim(void)
{
    return CLF_DIM;
}

const char *net_clf_classify(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return NULL;
    }

    /* 1) 切字符：记下每个 codepoint 的起点与字节长度（ASCII 顺带转小写） */
    char low[CLF_MAX_CP * 4];
    const char *cp[CLF_MAX_CP];
    int cplen[CLF_MAX_CP];
    int ncp = 0;

    size_t li = 0;
    for (const char *p = text; *p != '\0' && ncp < CLF_MAX_CP;) {
        unsigned char c = (unsigned char)*p;
        int len = 1;
        if (c >= 0xF0) {
            len = 4;
        } else if (c >= 0xE0) {
            len = 3;
        } else if (c >= 0xC0) {
            len = 2;
        }
        if (p[len - 1] == '\0' || li + (size_t)len > sizeof(low)) {
            break;                                  /* 截断的多字节序列：停 */
        }
        cp[ncp] = low + li;
        cplen[ncp] = len;
        for (int k = 0; k < len; k++) {
            unsigned char ch = (unsigned char)p[k];
            if (len == 1) {
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                    goto next_cp_skip;              /* 空白不进特征（与训练一致） */
                }
                if (ch >= 'A' && ch <= 'Z') {
                    ch = (unsigned char)(ch - 'A' + 'a');
                }
            }
            low[li++] = (char)ch;
        }
        ncp++;
next_cp_skip:
        p += len;
    }
    if (ncp == 0) {
        return NULL;
    }

    /* 2) 稀疏特征：字符 1-3 gram 的 CRC-32 % DIM，计数累加 */
    static uint32_t fidx[CLF_MAX_FEAT];
    static uint16_t fcnt[CLF_MAX_FEAT];
    int nf = 0;
    for (int n = 1; n <= 3; n++) {
        for (int i = 0; i + n <= ncp; i++) {
            const char *start = cp[i];
            const char *end = cp[i + n - 1] + cplen[i + n - 1];
            uint32_t h = crc32_buf((const uint8_t *)start, (size_t)(end - start)) % CLF_DIM;

            int slot = -1;
            for (int k = 0; k < nf; k++) {
                if (fidx[k] == h) {
                    slot = k;
                    break;
                }
            }
            if (slot < 0) {
                if (nf >= CLF_MAX_FEAT) {
                    continue;
                }
                slot = nf++;
                fidx[slot] = h;
                fcnt[slot] = 0;
            }
            if (fcnt[slot] < 0xFFFF) {
                fcnt[slot]++;
            }
        }
    }
    if (nf == 0) {
        return NULL;
    }

    /* 3) L2 归一化（保序，但会让不同长度的句子可比） */
    double norm = 0.0;
    for (int k = 0; k < nf; k++) {
        norm += (double)fcnt[k] * (double)fcnt[k];
    }
    norm = sqrt(norm);
    if (norm <= 0.0) {
        return NULL;
    }

    /* 4) 稀疏点积 + argmax */
    int best = 0;
    double best_score = -1e300;
    for (int c = 0; c < CLF_CLASSES; c++) {
        double s = (double)CLF_B[c] * CLF_SCALE;
        const signed char *row = CLF_W[c];
        for (int k = 0; k < nf; k++) {
            s += ((double)fcnt[k] / norm) * ((double)row[fidx[k]] * (double)CLF_SCALE);
        }
        if (s > best_score) {
            best_score = s;
            best = c;
        }
    }
    return CLF_LABELS[best];
}
