#!/usr/bin/env python3
"""把 llama2.c 的 run.c 移植成 ESP-IDF 上能跑的 espllm_engine.c。

为什么用脚本而不是直接改文件：这份源码是第三方的（MIT），评审要能看清
"哪些是上游的、哪些是我们改的"。脚本让这段差别是**可复现、可审计**的：
对着固定的上游版本跑，产出固定的结果；上游没变，产出就不会变。

三处改动 + 一处截断，都是"设备上没有文件系统/没有 mmap"造成的：

  1. 去掉 <fcntl.h> / <sys/mman.h> 那几行，补上 <sys/types.h>（ssize_t 要用）
     和两个由我们实现的钩子声明。
  2. read_checkpoint  : 从"fopen + fseek + mmap"改成"直接吃一块内存"。
  3. build_transformer: 同上，参数换成内存指针。
  4. build_tokenizer  : 从 fopen/fread 改成从内存里按同样的格式解析。
  5. generate() 里两处输出改成 llm_emit()/llm_note_speed()，好让上层拿到文本
     和速度（HTTP 接口与日志都要用）。
  6. 截掉 read_stdin / chat / error_usage / main —— 那些是命令行程序的东西。

然后追加一段"引擎封装"（llm_engine_init/generate/text），它不属于上游。

用法：
    python tools/port_llama2.py           # 生成 components/espllm/espllm_engine.c
    python tools/port_llama2.py --check   # 只校验上游哈希，不写文件
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SRC = os.path.join(ROOT, "third_party", "llama2.c", "run.c")
DST = os.path.join(ROOT, "components", "espllm", "espllm_engine.c")

# 上游版本锁定：karpathy/llama2.c 的 run.c（38,545 字节）。换版本要先确认
# 下面这些锚点还成立，然后更新这个哈希。
EXPECT_SHA256 = "9c4f2d5c6ae01b71726d1cc37530d71e60bff0ec7cc012565f16a43c1ca658bd"
EXPECT_SIZE = 38545

PROVENANCE = """\
/* ---------------------------------------------------------------------------
 * AUTO-GENERATED -- do not edit by hand.
 *
 *   python tools/port_llama2.py
 *
 * from third_party/llama2.c/run.c (karpathy/llama2.c, MIT -- see
 * third_party/llama2.c/LICENSE), source sha256:
 *   {sha}
 *
 * Everything above the "ESP-IDF engine wrapper" banner is upstream code with
 * the file/stdio glue replaced (there is no mmap on the device); the wrapper
 * at the end is ours.  The script asserts every anchor it rewrites, so a
 * silent upstream change cannot produce a half-ported file.
 * ------------------------------------------------------------------------- */
"""

# --- 1. 头文件 ---------------------------------------------------------------
INCLUDES_FROM = """\
#include <fcntl.h>
#if defined _WIN32
    #include "win.h"
#else
    #include <unistd.h>
    #include <sys/mman.h>
#endif
"""

INCLUDES_TO = """\
/* The device has neither mmap() nor a filesystem holding the checkpoint: the
 * model and the tokenizer arrive as memory blobs from flash partitions. */
#include <sys/types.h>
#include <stdint.h>

/* Provided by the engine wrapper at the end of this file. */
void llm_emit(const char *piece);
void llm_note_speed(double tok_per_sec);
"""

# --- 2. read_checkpoint ------------------------------------------------------
READ_CHECKPOINT_FROM = """\
void read_checkpoint(char* checkpoint, Config* config, TransformerWeights* weights,
                     int* fd, float** data, ssize_t* file_size) {
    FILE *file = fopen(checkpoint, "rb");
    if (!file) { fprintf(stderr, "Couldn't open file %s\\n", checkpoint); exit(EXIT_FAILURE); }
    // read in the config header
    if (fread(config, sizeof(Config), 1, file) != 1) { exit(EXIT_FAILURE); }
    // negative vocab size is hacky way of signaling unshared weights. bit yikes.
    int shared_weights = config->vocab_size > 0 ? 1 : 0;
    config->vocab_size = abs(config->vocab_size);
    // figure out the file size
    fseek(file, 0, SEEK_END); // move file pointer to end of file
    *file_size = ftell(file); // get the file size, in bytes
    fclose(file);
    // memory map the Transformer weights into the data pointer
    *fd = open(checkpoint, O_RDONLY); // open in read only mode
    if (*fd == -1) { fprintf(stderr, "open failed!\\n"); exit(EXIT_FAILURE); }
    *data = mmap(NULL, *file_size, PROT_READ, MAP_PRIVATE, *fd, 0);
    if (*data == MAP_FAILED) { fprintf(stderr, "mmap failed!\\n"); exit(EXIT_FAILURE); }
    float* weights_ptr = *data + sizeof(Config)/sizeof(float);
    memory_map_weights(weights, config, weights_ptr, shared_weights);
}
"""

READ_CHECKPOINT_TO = """\
/* ESP-IDF: the checkpoint is already in memory (a flash partition mapped by
 * llm_model.c, or a PSRAM copy of one), so there is nothing to read here --
 * parse the header and point the weights at what follows it.  Returns 0 on
 * success instead of calling exit(), so a bad blob is a log line rather than
 * a reboot loop. */
int read_checkpoint_blob(const void *blob, size_t blob_size,
                         Config* config, TransformerWeights* weights) {
    if (blob == NULL || blob_size < sizeof(Config)) {
        fprintf(stderr, "model blob too small: %u bytes\\n", (unsigned)blob_size);
        return -1;
    }
    memcpy(config, blob, sizeof(Config));
    // negative vocab size is hacky way of signaling unshared weights. bit yikes.
    int shared_weights = config->vocab_size > 0 ? 1 : 0;
    config->vocab_size = abs(config->vocab_size);
    if (config->vocab_size <= 0 || config->dim <= 0 || config->n_layers <= 0) {
        fprintf(stderr, "model header looks wrong (vocab=%d dim=%d layers=%d)\\n",
                config->vocab_size, config->dim, config->n_layers);
        return -1;
    }
    float* weights_ptr = (float*)((const char*)blob + sizeof(Config));
    memory_map_weights(weights, config, weights_ptr, shared_weights);
    return 0;
}
"""

# --- 3. build_transformer / free_transformer ---------------------------------
BUILD_TRANSFORMER_FROM = """\
void build_transformer(Transformer *t, char* checkpoint_path) {
    // read in the Config and the Weights from the checkpoint
    read_checkpoint(checkpoint_path, &t->config, &t->weights, &t->fd, &t->data, &t->file_size);
    // allocate the RunState buffers
    malloc_run_state(&t->state, &t->config);
}

void free_transformer(Transformer* t) {
    // close the memory mapping
    if (t->data != MAP_FAILED) { munmap(t->data, t->file_size); }
    if (t->fd != -1) { close(t->fd); }
    // free the RunState buffers
    free_run_state(&t->state);
}
"""

BUILD_TRANSFORMER_TO = """\
int build_transformer_blob(Transformer *t, const void *blob, size_t blob_size) {
    // read in the Config and the Weights from the in-memory checkpoint
    if (read_checkpoint_blob(blob, blob_size, &t->config, &t->weights) != 0) { return -1; }
    // allocate the RunState buffers
    malloc_run_state(&t->state, &t->config);
    return 0;
}

void free_transformer(Transformer* t) {
    // the weights are not ours to unmap here: llm_model.c owns that mapping
    free_run_state(&t->state);
}
"""

# --- 4. build_tokenizer ------------------------------------------------------
TOKENIZER_FROM = """\
void build_tokenizer(Tokenizer* t, char* tokenizer_path, int vocab_size) {
    // i should have written the vocab_size into the tokenizer file... sigh
    t->vocab_size = vocab_size;
    // malloc space to hold the scores and the strings
    t->vocab = (char**)malloc(vocab_size * sizeof(char*));
    t->vocab_scores = (float*)malloc(vocab_size * sizeof(float));
    t->sorted_vocab = NULL; // initialized lazily
    for (int i = 0; i < 256; i++) {
        t->byte_pieces[i * 2] = (unsigned char)i;
        t->byte_pieces[i * 2 + 1] = '\\0';
    }
    // read in the file
    FILE *file = fopen(tokenizer_path, "rb");
    if (!file) { fprintf(stderr, "couldn't load %s\\n", tokenizer_path); exit(EXIT_FAILURE); }
    if (fread(&t->max_token_length, sizeof(int), 1, file) != 1) { fprintf(stderr, "failed read\\n"); exit(EXIT_FAILURE); }
    int len;
    for (int i = 0; i < vocab_size; i++) {
        if (fread(t->vocab_scores + i, sizeof(float), 1, file) != 1) { fprintf(stderr, "failed read\\n"); exit(EXIT_FAILURE);}
        if (fread(&len, sizeof(int), 1, file) != 1) { fprintf(stderr, "failed read\\n"); exit(EXIT_FAILURE); }
        t->vocab[i] = (char *)malloc(len + 1);
        if (fread(t->vocab[i], len, 1, file) != 1) { fprintf(stderr, "failed read\\n"); exit(EXIT_FAILURE); }
        t->vocab[i][len] = '\\0'; // add the string terminating token
    }
    fclose(file);
}
"""

TOKENIZER_TO = """\
/* ESP-IDF: same byte layout as the file upstream reads, walked over a memory
 * blob instead.  Bounds-checked, and it returns -1 rather than exiting. */
int build_tokenizer_blob(Tokenizer* t, const void *blob, size_t blob_size, int vocab_size) {
    // i should have written the vocab_size into the tokenizer file... sigh
    t->vocab_size = vocab_size;
    t->vocab = (char**)malloc(vocab_size * sizeof(char*));
    t->vocab_scores = (float*)malloc(vocab_size * sizeof(float));
    t->sorted_vocab = NULL; // initialized lazily
    if (t->vocab == NULL || t->vocab_scores == NULL) {
        fprintf(stderr, "tokenizer: out of memory for %d entries\\n", vocab_size);
        return -1;
    }
    for (int i = 0; i < 256; i++) {
        t->byte_pieces[i * 2] = (unsigned char)i;
        t->byte_pieces[i * 2 + 1] = '\\0';
    }
    const char *p = (const char*)blob;
    const char *end = p + blob_size;
    if (p + sizeof(int) > end) { fprintf(stderr, "tokenizer blob too small\\n"); return -1; }
    memcpy(&t->max_token_length, p, sizeof(int));
    p += sizeof(int);
    int len;
    for (int i = 0; i < vocab_size; i++) {
        if (p + sizeof(float) + sizeof(int) > end) {
            fprintf(stderr, "tokenizer truncated at entry %d\\n", i);
            return -1;
        }
        memcpy(t->vocab_scores + i, p, sizeof(float));
        p += sizeof(float);
        memcpy(&len, p, sizeof(int));
        p += sizeof(int);
        if (len < 0 || p + len > end) {
            fprintf(stderr, "tokenizer entry %d has bad length %d\\n", i, len);
            return -1;
        }
        t->vocab[i] = (char *)malloc(len + 1);
        if (t->vocab[i] == NULL) { fprintf(stderr, "tokenizer: oom at %d\\n", i); return -1; }
        memcpy(t->vocab[i], p, len);
        p += len;
        t->vocab[i][len] = '\\0'; // add the string terminating token
    }
    return 0;
}
"""

# --- 5. generate() 的两处输出 ------------------------------------------------
EMIT_FROM = """\
        char* piece = decode(tokenizer, token, next);
        safe_printf(piece); // same as printf("%s", piece), but skips "unsafe" bytes
        fflush(stdout);
"""

EMIT_TO = """\
        char* piece = decode(tokenizer, token, next);
        llm_emit(piece); // hands the piece to the caller instead of stdout
"""

NEWLINE_FROM = """\
    }
    printf("\\n");

    // report achieved tok/s (pos-1 because the timer starts after first iteration)
    if (pos > 1) {
        long end = time_in_ms();
        fprintf(stderr, "achieved tok/s: %f\\n", (pos-1) / (double)(end-start)*1000);
    }
"""

NEWLINE_TO = """\
    }
    llm_emit("\\n");

    // report achieved tok/s (pos-1 because the timer starts after first iteration)
    if (pos > 1) {
        long end = time_in_ms();
        double tok_per_sec = (pos-1) / (double)(end-start)*1000;
        fprintf(stderr, "achieved tok/s: %f\\n", tok_per_sec);
        llm_note_speed(tok_per_sec);
    }
"""

# --- 6. OpenMP pragmas -------------------------------------------------------
# Upstream puts `#pragma omp parallel for` on the two hot loops.  Without
# -fopenmp they are inert, but ESP-IDF compiles with -Werror=all, where an
# unknown pragma is an error.  Dropping them is honest: this build really is
# single-threaded (the S3's second core is idle -- splitting matmul over both
# cores is future work, and it would live here rather than in a pragma).
OMP_MATMUL_FROM = "    #pragma omp parallel for private(i)\n"
OMP_MATMUL_TO = ("    /* ESP-IDF: no OpenMP -- single core, see tools/port_llama2.py */\n")

OMP_ATTN_FROM = "        #pragma omp parallel for private(h)\n"
OMP_ATTN_TO = "        /* ESP-IDF: no OpenMP (see the note in matmul) */\n"

# --- 7. 截断点 ---------------------------------------------------------------
TRUNCATE_AT = "void read_stdin(const char* guide, char* buffer, size_t bufsize) {"

ENGINE_TAIL = r"""

/* ===========================================================================
 * ESP-IDF engine wrapper -- ours, not upstream llama2.c.
 *
 * Single-threaded by design: the caller serializes access (the HTTP handler
 * takes a mutex).  Text lands in a fixed buffer; overflow sets a flag rather
 * than truncating silently.
 * =========================================================================== */

#define LLM_TEXT_MAX 8192

/* 权重量化格式，写在打包头第 12~15 字节（fetch_model.py 留的 4 个保留字节）。
 * 见 tools/quantize_model.py；0 就是上游的 fp32 检查点。 */
#define LLM_QFMT_FP32 0
#define LLM_QFMT_INT8 1
#define LLM_QFMT_INT4 2

static char s_text[LLM_TEXT_MAX];
static size_t s_text_len;
static int s_text_truncated;
static double s_tok_per_sec;

static Transformer s_transformer;
static Tokenizer s_tokenizer;
static Sampler s_sampler;
static int s_ready;
static int s_format = LLM_QFMT_FP32;

void llm_emit(const char *piece)
{
    size_t n = strlen(piece);
    if (s_text_len + n + 1 > sizeof(s_text)) {
        s_text_truncated = 1;
        return;
    }
    memcpy(s_text + s_text_len, piece, n);
    s_text_len += n;
    s_text[s_text_len] = '\0';
}

void llm_note_speed(double tok_per_sec)
{
    s_tok_per_sec = tok_per_sec;
}

/* ---------------------------------------------------------------------------
 * Quantized weights -- ours, not upstream.
 *
 * Upstream run.c only knows fp32 weights.  The quantized formats (see
 * tools/quantize_model.py) keep each weight matrix row-major as int8 or int4
 * with one fp32 scale per output row, and quantize the activation vector on
 * the fly before every matmul.  That is the shape llama2.c's runq.c uses, minus
 * its group-size rule: a group there has to divide every dimension, and
 * stories260K's hidden_dim of 172 is a multiple of neither 32 nor 64, so a
 * group would quietly leave a tail out of the dot product.
 *
 * Layout (format 1 = int8, 2 = int4), everything after the 28-byte Config:
 *     fp32 rms_att (layers*dim), fp32 rms_ffn (layers*dim), fp32 rms_final (dim)
 *     token_embedding  rows=vocab,  cols=dim
 *     per layer        wq(dim,dim) wk(kv,dim) wv(kv,dim) wo(dim,dim)
 *                      w1(hidden,dim) w2(dim,hidden) w3(hidden,dim)
 *     wcls             rows=vocab, cols=dim   (only when not shared)
 * Each tensor is its packed weights followed by `rows` fp32 scales.  The
 * packer and this mapper derive the total size from the header independently,
 * so a layout drift is a refused blob rather than wrong numbers.
 * ------------------------------------------------------------------------- */

typedef struct {
    const unsigned char *q;   /* int8: one byte per weight; int4: two per byte */
    const float *s;           /* one scale per row */
    int layers;               /* how many layers are stacked here (1 for tok/wcls) */
    int rows;                 /* output features *per layer* */
    int cols;                 /* inputs reduced over */
    int bits;
} llm_qweight_t;

typedef struct {
    float *rms_att;
    float *rms_ffn;
    float *rms_final;
    llm_qweight_t tok;
    llm_qweight_t wq, wk, wv, wo, w1, w2, w3, wcls;
} llm_qweights_t;

static llm_qweights_t s_qw;
static int8_t *s_xq;          /* the quantized activation vector */
static int s_xq_cap;

/* Bytes the quantized part of the model takes.  Mirrors quant_size() in
 * tools/quantize_model.py; keep the two in step. */
static size_t q_bytes_of(const Config *p, int fmt, int shared)
{
    const int bits = (fmt == LLM_QFMT_INT8) ? 8 : 4;
    const int dim = p->dim, hidden = p->hidden_dim, L = p->n_layers, V = p->vocab_size;
    const int kv = (dim * p->n_kv_heads) / p->n_heads;
    size_t n = 4u * (size_t)(2 * L * dim + dim);
    n += (size_t)V * dim * bits / 8 + 4u * (size_t)V;
    n += (size_t)L * dim * dim * bits / 8 + 4u * (size_t)L * dim;        /* wq */
    n += (size_t)L * kv * dim * bits / 8 + 4u * (size_t)L * kv;          /* wk */
    n += (size_t)L * kv * dim * bits / 8 + 4u * (size_t)L * kv;          /* wv */
    n += (size_t)L * dim * dim * bits / 8 + 4u * (size_t)L * dim;        /* wo */
    n += (size_t)L * dim * hidden * bits / 8 + 4u * (size_t)L * hidden;  /* w1 */
    n += (size_t)L * hidden * dim * bits / 8 + 4u * (size_t)L * dim;     /* w2 */
    n += (size_t)L * dim * hidden * bits / 8 + 4u * (size_t)L * hidden;  /* w3 */
    if (!shared) {
        n += (size_t)V * dim * bits / 8 + 4u * (size_t)V;                /* wcls */
    }
    return n;
}

/* Takes one tensor out of the byte stream and advances past it.  NULL when the
 * blob cannot be walked that way: int4 with an odd column count, or a packed
 * tensor whose size would leave the scales off a 4-byte boundary. */
static const unsigned char *q_take(llm_qweight_t *w, const unsigned char *p,
                                   int layers, int rows, int cols, int bits)
{
    if (bits == 4 && (cols & 1) != 0) {
        fprintf(stderr, "int4 needs an even column count, got %d\n", cols);
        return NULL;
    }
    const size_t bytes = (size_t)layers * (size_t)rows * (size_t)cols * (size_t)bits / 8;
    if ((bytes & 3u) != 0) {
        fprintf(stderr, "quantized %d layers x %d x %d at %d bits takes %u B, "
                        "not a multiple of 4\n",
                layers, rows, cols, bits, (unsigned)bytes);
        return NULL;
    }
    w->q = p;
    w->layers = layers;
    w->rows = rows;
    w->cols = cols;
    w->bits = bits;
    p += bytes;
    w->s = (const float *)p;
    return p + 4u * (size_t)layers * (size_t)rows;
}

/* Fills in the Config and points every weight at its place in the blob. */
static int q_map_weights(const void *blob, size_t blob_size, int fmt, Config *cfg)
{
    if (blob == NULL || blob_size < sizeof(Config)) {
        fprintf(stderr, "quantized model blob too small: %u bytes\n", (unsigned)blob_size);
        return -1;
    }
    if (((uintptr_t)blob & 3u) != 0) {
        fprintf(stderr, "quantized model blob is not 4-byte aligned\n");
        return -1;
    }
    memcpy(cfg, blob, sizeof(Config));
    const int shared = cfg->vocab_size > 0 ? 1 : 0;
    cfg->vocab_size = abs(cfg->vocab_size);
    if (cfg->vocab_size <= 0 || cfg->dim <= 0 || cfg->hidden_dim <= 0 || cfg->n_layers <= 0
        || cfg->n_heads <= 0 || cfg->n_kv_heads <= 0 || cfg->dim % cfg->n_heads != 0) {
        fprintf(stderr, "quantized header looks wrong (vocab=%d dim=%d hidden=%d layers=%d)\n",
                cfg->vocab_size, cfg->dim, cfg->hidden_dim, cfg->n_layers);
        return -1;
    }
    const int bits = (fmt == LLM_QFMT_INT8) ? 8 : 4;
    const int dim = cfg->dim, hidden = cfg->hidden_dim, L = cfg->n_layers, V = cfg->vocab_size;
    const int kv = (dim * cfg->n_kv_heads) / cfg->n_heads;

    const size_t need = sizeof(Config) + q_bytes_of(cfg, fmt, shared);
    if (blob_size < need) {
        fprintf(stderr, "quantized blob is %u B but the header describes %u B -- "
                        "was the right file flashed?\n", (unsigned)blob_size, (unsigned)need);
        return -1;
    }

    const unsigned char *p = (const unsigned char *)blob + sizeof(Config);
    s_qw.rms_att = (float *)p;         p += 4u * (size_t)L * dim;
    s_qw.rms_ffn = (float *)p;         p += 4u * (size_t)L * dim;
    s_qw.rms_final = (float *)p;       p += 4u * dim;
    if ((p = q_take(&s_qw.tok, p, 1, V, dim, bits)) == NULL) { return -1; }
    if ((p = q_take(&s_qw.wq, p, L, dim, dim, bits)) == NULL) { return -1; }
    if ((p = q_take(&s_qw.wk, p, L, kv, dim, bits)) == NULL) { return -1; }
    if ((p = q_take(&s_qw.wv, p, L, kv, dim, bits)) == NULL) { return -1; }
    if ((p = q_take(&s_qw.wo, p, L, dim, dim, bits)) == NULL) { return -1; }
    if ((p = q_take(&s_qw.w1, p, L, hidden, dim, bits)) == NULL) { return -1; }
    if ((p = q_take(&s_qw.w2, p, L, dim, hidden, bits)) == NULL) { return -1; }
    if ((p = q_take(&s_qw.w3, p, L, hidden, dim, bits)) == NULL) { return -1; }
    if (shared) {
        s_qw.wcls = s_qw.tok;   /* a shared classifier is the embedding table */
    } else if ((p = q_take(&s_qw.wcls, p, 1, V, dim, bits)) == NULL) {
        return -1;
    }
    return 0;
}

/* Packed bytes in one row of a tensor. */
static size_t q_row_bytes(const llm_qweight_t *w)
{
    return (size_t)w->cols * (size_t)w->bits / 8;
}

/* The `row`-th output row of tensor `w` in layer `l`.  `w->rows` counts one
 * layer's rows and the layers follow each other in the blob, so the layer
 * index multiplies it; the scale array is laid out the same way, which is why
 * one index serves both.  (Getting this wrong costs nothing at layer 0 and
 * garbage after it -- that is exactly how the first version of this code
 * behaved.) */
static const unsigned char *q_row(const llm_qweight_t *w, int l, int row)
{
    return w->q + ((size_t)l * (size_t)w->rows + (size_t)row) * q_row_bytes(w);
}

static float q_scale(const llm_qweight_t *w, int l, int row)
{
    return w->s[(size_t)l * (size_t)w->rows + (size_t)row];
}

/* int8 x int8 dot product, accumulated in int32.  Worst case here is
 * 127 * 127 * 512 = 8.3e6, three orders of magnitude inside int32. */
static int32_t q_dot(const unsigned char *row, const int8_t *xq, int n, int bits)
{
    int32_t acc = 0;
    if (bits == 8) {
        const int8_t *r = (const int8_t *)row;
        for (int j = 0; j < n; j++) {
            acc += (int32_t)xq[j] * (int32_t)r[j];
        }
    } else {
        /* two weights per byte, low nibble first (same order the packer used) */
        for (int j = 0; j < n; j += 2) {
            const unsigned char b = row[j >> 1];
            acc += (int32_t)xq[j] * ((int32_t)(b & 0x0F) - 8);
            acc += (int32_t)xq[j + 1] * ((int32_t)(b >> 4) - 8);
        }
    }
    return acc;
}

/* xout (rows,) = xs * row_scale * (x . row), with x quantized to int8 first. */
static void matmul_q(float *xout, const llm_qweight_t *w, int l, const float *x)
{
    const int n = w->cols;
    if (n > s_xq_cap) {   /* only reachable if the header and the map disagreed */
        memset(xout, 0, (size_t)w->rows * sizeof(float));
        return;
    }
    float amax = 0.0f;
    for (int j = 0; j < n; j++) {
        const float a = fabsf(x[j]);
        if (a > amax) { amax = a; }
    }
    const float xs = amax > 0.0f ? amax / 127.0f : 1.0f;
    if (amax > 0.0f) {
        for (int j = 0; j < n; j++) {
            s_xq[j] = (int8_t)lroundf(x[j] / xs);
        }
    } else {
        memset(s_xq, 0, (size_t)n);
    }
    for (int i = 0; i < w->rows; i++) {
        xout[i] = xs * q_scale(w, l, i) * (float)q_dot(q_row(w, l, i), s_xq, n, w->bits);
    }
}

/* One row of the quantized embedding table, back to fp32 activations. */
static void q_dequant_row(float *x, const llm_qweight_t *w, int row)
{
    const int n = w->cols;
    const float s = q_scale(w, 0, row);
    if (w->bits == 8) {
        const int8_t *r = (const int8_t *)q_row(w, 0, row);
        for (int j = 0; j < n; j++) {
            x[j] = (float)r[j] * s;
        }
    } else {
        const unsigned char *r = q_row(w, 0, row);
        for (int j = 0; j < n; j += 2) {
            const unsigned char b = r[j >> 1];
            x[j] = (float)((int32_t)(b & 0x0F) - 8) * s;
            x[j + 1] = (float)((int32_t)(b >> 4) - 8) * s;
        }
    }
}

/* The same data flow as upstream forward(), with the weights read through the
 * quantized tensors instead.  It lives here rather than in the ported region
 * because upstream has no such variant, and the ported region has to stay
 * byte-identical to the file it was generated from. */
static float *forward_q(Transformer *transformer, int token, int pos)
{
    Config *p = &transformer->config;
    RunState *s = &transformer->state;
    float *x = s->x;
    const int dim = p->dim;
    const int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    const int kv_mul = p->n_heads / p->n_kv_heads;
    const int hidden_dim = p->hidden_dim;
    const int head_size = dim / p->n_heads;

    q_dequant_row(x, &s_qw.tok, token);

    for (int l = 0; l < p->n_layers; l++) {

        rmsnorm(s->xb, x, s_qw.rms_att + l * dim, dim);

        /* key and value point at the kv cache, as in the ported forward() */
        const int loff = l * p->seq_len * kv_dim;
        s->k = s->key_cache + loff + pos * kv_dim;
        s->v = s->value_cache + loff + pos * kv_dim;

        matmul_q(s->q, &s_qw.wq, l, s->xb);
        matmul_q(s->k, &s_qw.wk, l, s->xb);
        matmul_q(s->v, &s_qw.wv, l, s->xb);

        /* RoPE relative positional encoding: complex-valued rotate q and k in each head */
        for (int i = 0; i < dim; i += 2) {
            const int head_dim = i % head_size;
            const float freq = 1.0f / powf(10000.0f, head_dim / (float)head_size);
            const float val = pos * freq;
            const float fcr = cosf(val);
            const float fci = sinf(val);
            const int rotn = i < kv_dim ? 2 : 1; /* 2 = q & k, 1 = q only */
            for (int v = 0; v < rotn; v++) {
                float *vec = v == 0 ? s->q : s->k;
                const float v0 = vec[i];
                const float v1 = vec[i + 1];
                vec[i] = v0 * fcr - v1 * fci;
                vec[i + 1] = v0 * fci + v1 * fcr;
            }
        }

        for (int h = 0; h < p->n_heads; h++) {
            float *q = s->q + h * head_size;
            float *att = s->att + h * p->seq_len;
            for (int t = 0; t <= pos; t++) {
                const float *k = s->key_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                float score = 0.0f;
                for (int i = 0; i < head_size; i++) {
                    score += q[i] * k[i];
                }
                att[t] = score / sqrtf(head_size);
            }

            softmax(att, pos + 1);

            float *xb = s->xb + h * head_size;
            memset(xb, 0, (size_t)head_size * sizeof(float));
            for (int t = 0; t <= pos; t++) {
                const float *v = s->value_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                const float a = att[t];
                for (int i = 0; i < head_size; i++) {
                    xb[i] += a * v[i];
                }
            }
        }

        matmul_q(s->xb2, &s_qw.wo, l, s->xb);

        for (int i = 0; i < dim; i++) {
            x[i] += s->xb2[i];
        }

        rmsnorm(s->xb, x, s_qw.rms_ffn + l * dim, dim);

        matmul_q(s->hb, &s_qw.w1, l, s->xb);
        matmul_q(s->hb2, &s_qw.w3, l, s->xb);

        for (int i = 0; i < hidden_dim; i++) {
            float val = s->hb[i];
            val *= (1.0f / (1.0f + expf(-val)));
            val *= s->hb2[i];
            s->hb[i] = val;
        }

        matmul_q(s->xb, &s_qw.w2, l, s->hb);

        for (int i = 0; i < dim; i++) {
            x[i] += s->xb[i];
        }
    }

    rmsnorm(x, x, s_qw.rms_final, dim);
    matmul_q(s->logits, &s_qw.wcls, 0, x);
    return s->logits;
}

/* generate() over forward_q(): same loop and same reporting, different
 * forward pass. */
static void generate_q(Transformer *transformer, Tokenizer *tokenizer, Sampler *sampler,
                       char *prompt, int steps)
{
    int num_prompt_tokens = 0;
    int *prompt_tokens = (int *)malloc((strlen(prompt) + 3) * sizeof(int));
    if (prompt_tokens == NULL) { return; }
    encode(tokenizer, prompt, 1, 0, prompt_tokens, &num_prompt_tokens);
    if (num_prompt_tokens < 1) {
        free(prompt_tokens);
        return;
    }

    long start = 0;  // used to time our code, only initialized after first iteration
    int next;        // will store the next token in the sequence
    int token = prompt_tokens[0]; // kick off with the first token in the prompt
    int pos = 0;     // position in the sequence
    while (pos < steps) {

        float *logits = forward_q(transformer, token, pos);

        if (pos < num_prompt_tokens - 1) {
            next = prompt_tokens[pos + 1];
        } else {
            next = sample(sampler, logits);
        }
        pos++;

        if (next == 1) { break; }

        char *piece = decode(tokenizer, token, next);
        llm_emit(piece);
        token = next;

        if (start == 0) { start = time_in_ms(); }
    }
    llm_emit("\n");

    if (pos > 1) {
        long end = time_in_ms();
        double tok_per_sec = (pos - 1) / (double)(end - start) * 1000;
        fprintf(stderr, "achieved tok/s: %f\n", tok_per_sec);
        llm_note_speed(tok_per_sec);
    }
    free(prompt_tokens);
}

/* Returns 0 on success, negative on a bad blob. */
int llm_engine_init(const void *model_blob, size_t model_size,
                    const void *tokenizer_blob, size_t tokenizer_size,
                    int format,
                    float temperature, float topp, unsigned long long seed)
{
    if (s_ready) { return 0; }
    if (format == LLM_QFMT_FP32) {
        if (build_transformer_blob(&s_transformer, model_blob, model_size) != 0) { return -1; }
    } else {
        if (q_map_weights(model_blob, model_size, format, &s_transformer.config) != 0) { return -1; }
        malloc_run_state(&s_transformer.state, &s_transformer.config);
        /* 激活向量按整条量化，缓冲区只要放得下最长的那个输入向量 */
        int cap = s_transformer.config.dim > s_transformer.config.hidden_dim
                ? s_transformer.config.dim : s_transformer.config.hidden_dim;
        s_xq = (int8_t *)malloc((size_t)cap + 8);
        if (s_xq == NULL) { return -1; }
        s_xq_cap = cap;
    }
    if (build_tokenizer_blob(&s_tokenizer, tokenizer_blob, tokenizer_size,
                             s_transformer.config.vocab_size) != 0) { return -2; }
    build_sampler(&s_sampler, s_transformer.config.vocab_size, temperature, topp, seed);
    s_format = format;
    s_ready = 1;
    return 0;
}

int llm_engine_ready(void) { return s_ready; }

const Config *llm_engine_config(void) { return &s_transformer.config; }

int llm_engine_format(void) { return s_format; }

const char *llm_engine_format_name(void)
{
    switch (s_format) {
    case LLM_QFMT_INT8: return "int8 quantized";
    case LLM_QFMT_INT4: return "int4 quantized";
    default:            return "fp32";
    }
}

/* Sampling is a knob, not a constant: a request may pin temperature 0 to get
 * the greedy, reproducible run that makes two weight formats comparable. */
void llm_engine_set_temperature(float temperature) { s_sampler.temperature = temperature; }

/* Generates up to max_new_tokens after the prompt.  The decoded text is in
 * llm_engine_text() afterwards; the prompt itself is not echoed. */
int llm_engine_generate(const char *prompt, int max_new_tokens)
{
    if (!s_ready) { return -1; }
    s_text_len = 0;
    s_text[0] = '\0';
    s_text_truncated = 0;
    s_tok_per_sec = 0.0;
    /* generate() walks pos from 0 and forward() indexes the KV cache with it,
     * so pos must never reach config.seq_len -- the cache is exactly that long
     * and upstream has no bounds check.  Counting the prompt's tokens here
     * keeps max_new_tokens exact *and* the walk in bounds. */
    int prompt_tokens = 0;
    int *scratch = (int *)malloc((strlen(prompt) + 3) * sizeof(int));
    if (scratch == NULL) { return -1; }
    encode(&s_tokenizer, (char *)prompt, 1, 0, scratch, &prompt_tokens);
    free(scratch);

    int steps = prompt_tokens + max_new_tokens;
    if (steps > s_transformer.config.seq_len) { steps = s_transformer.config.seq_len; }
    if (steps <= prompt_tokens) {
        fprintf(stderr, "prompt fills the %d-token context; nothing left to generate\n",
                s_transformer.config.seq_len);
        return -1;
    }
    if (s_format == LLM_QFMT_FP32) {
        generate(&s_transformer, &s_tokenizer, &s_sampler, (char *)prompt, steps);
    } else {
        generate_q(&s_transformer, &s_tokenizer, &s_sampler, (char *)prompt, steps);
    }
    return 0;
}

const char *llm_engine_text(void) { return s_text; }
size_t llm_engine_text_len(void) { return s_text_len; }
int llm_engine_text_truncated(void) { return s_text_truncated; }
double llm_engine_tok_per_sec(void) { return s_tok_per_sec; }
"""


def main():
    check_only = "--check" in sys.argv

    with open(SRC, "rb") as f:
        src = f.read()
    sha = hashlib.sha256(src).hexdigest()

    print(f"upstream : {os.path.relpath(SRC, ROOT)}  {len(src):,} B  sha256 {sha[:16]}…")
    if len(src) != EXPECT_SIZE or sha != EXPECT_SHA256:
        print("!! 上游文件与锁定的版本不一致。")
        print(f"   期望 {EXPECT_SIZE:,} B / {EXPECT_SHA256}")
        print(f"   实际 {len(src):,} B / {sha}")
        print("   换上游版本请先逐条确认下面这些锚点仍然成立，再更新 EXPECT_*。")
        return 1
    if check_only:
        print("哈希一致，未写文件（--check）。")
        return 0

    text = src.decode("utf-8")

    pairs = [
        ("includes", INCLUDES_FROM, INCLUDES_TO),
        ("read_checkpoint", READ_CHECKPOINT_FROM, READ_CHECKPOINT_TO),
        ("build_transformer", BUILD_TRANSFORMER_FROM, BUILD_TRANSFORMER_TO),
        ("build_tokenizer", TOKENIZER_FROM, TOKENIZER_TO),
        ("generate/emit", EMIT_FROM, EMIT_TO),
        ("generate/newline", NEWLINE_FROM, NEWLINE_TO),
        ("omp/matmul", OMP_MATMUL_FROM, OMP_MATMUL_TO),
        ("omp/attention", OMP_ATTN_FROM, OMP_ATTN_TO),
    ]

    for name, old, new in pairs:
        count = text.count(old)
        if count != 1:
            print(f"!! 锚点 {name} 命中 {count} 次（应为 1 次）——上游可能变了，中止。")
            return 1
        text = text.replace(old, new)
        print(f"   改写 {name:<18} ok")

    idx = text.find(TRUNCATE_AT)
    if idx < 0:
        print("!! 找不到截断点（read_stdin），中止。")
        return 1
    print(f"   截断于 {TRUNCATE_AT[:32]}… 之前（去掉 chat/error_usage/main）")
    text = text[:idx]

    out = PROVENANCE.format(sha=sha) + text + ENGINE_TAIL

    os.makedirs(os.path.dirname(DST), exist_ok=True)
    with open(DST, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)

    print(f"生成     : {os.path.relpath(DST, ROOT)}  {len(out):,} B")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
