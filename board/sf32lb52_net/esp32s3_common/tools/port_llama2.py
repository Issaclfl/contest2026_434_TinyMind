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

static char s_text[LLM_TEXT_MAX];
static size_t s_text_len;
static int s_text_truncated;
static double s_tok_per_sec;

static Transformer s_transformer;
static Tokenizer s_tokenizer;
static Sampler s_sampler;
static int s_ready;

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

/* Returns 0 on success, negative on a bad blob. */
int llm_engine_init(const void *model_blob, size_t model_size,
                    const void *tokenizer_blob, size_t tokenizer_size,
                    float temperature, float topp, unsigned long long seed)
{
    if (s_ready) { return 0; }
    if (build_transformer_blob(&s_transformer, model_blob, model_size) != 0) { return -1; }
    if (build_tokenizer_blob(&s_tokenizer, tokenizer_blob, tokenizer_size,
                             s_transformer.config.vocab_size) != 0) { return -2; }
    build_sampler(&s_sampler, s_transformer.config.vocab_size, temperature, topp, seed);
    s_ready = 1;
    return 0;
}

int llm_engine_ready(void) { return s_ready; }

const Config *llm_engine_config(void) { return &s_transformer.config; }

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
    generate(&s_transformer, &s_tokenizer, &s_sampler, (char *)prompt, steps);
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
