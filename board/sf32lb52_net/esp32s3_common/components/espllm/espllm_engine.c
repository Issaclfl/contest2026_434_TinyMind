/* ---------------------------------------------------------------------------
 * AUTO-GENERATED -- do not edit by hand.
 *
 *   python tools/port_llama2.py
 *
 * from third_party/llama2.c/run.c (karpathy/llama2.c, MIT -- see
 * third_party/llama2.c/LICENSE), source sha256:
 *   9c4f2d5c6ae01b71726d1cc37530d71e60bff0ec7cc012565f16a43c1ca658bd
 *
 * Everything above the "ESP-IDF engine wrapper" banner is upstream code with
 * the file/stdio glue replaced (there is no mmap on the device); the wrapper
 * at the end is ours.  The script asserts every anchor it rewrites, so a
 * silent upstream change cannot produce a half-ported file.
 * ------------------------------------------------------------------------- */
/* Inference for Llama-2 Transformer model in pure C */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <string.h>
/* The device has neither mmap() nor a filesystem holding the checkpoint: the
 * model and the tokenizer arrive as memory blobs from flash partitions. */
#include <sys/types.h>
#include <stdint.h>

/* Provided by the engine wrapper at the end of this file. */
void llm_emit(const char *piece);
void llm_note_speed(double tok_per_sec);
// ----------------------------------------------------------------------------
// Transformer model

typedef struct {
    int dim; // transformer dimension
    int hidden_dim; // for ffn layers
    int n_layers; // number of layers
    int n_heads; // number of query heads
    int n_kv_heads; // number of key/value heads (can be < query heads because of multiquery)
    int vocab_size; // vocabulary size, usually 256 (byte-level)
    int seq_len; // max sequence length
} Config;

typedef struct {
    // token embedding table
    float* token_embedding_table;    // (vocab_size, dim)
    // weights for rmsnorms
    float* rms_att_weight; // (layer, dim) rmsnorm weights
    float* rms_ffn_weight; // (layer, dim)
    // weights for matmuls. note dim == n_heads * head_size
    float* wq; // (layer, dim, n_heads * head_size)
    float* wk; // (layer, dim, n_kv_heads * head_size)
    float* wv; // (layer, dim, n_kv_heads * head_size)
    float* wo; // (layer, n_heads * head_size, dim)
    // weights for ffn
    float* w1; // (layer, hidden_dim, dim)
    float* w2; // (layer, dim, hidden_dim)
    float* w3; // (layer, hidden_dim, dim)
    // final rmsnorm
    float* rms_final_weight; // (dim,)
    // (optional) classifier weights for the logits, on the last layer
    float* wcls;
} TransformerWeights;

typedef struct {
    // current wave of activations
    float *x; // activation at current time stamp (dim,)
    float *xb; // same, but inside a residual branch (dim,)
    float *xb2; // an additional buffer just for convenience (dim,)
    float *hb; // buffer for hidden dimension in the ffn (hidden_dim,)
    float *hb2; // buffer for hidden dimension in the ffn (hidden_dim,)
    float *q; // query (dim,)
    float *k; // key (dim,)
    float *v; // value (dim,)
    float *att; // buffer for scores/attention values (n_heads, seq_len)
    float *logits; // output logits
    // kv cache
    float* key_cache;   // (layer, seq_len, dim)
    float* value_cache; // (layer, seq_len, dim)
} RunState;

typedef struct {
    Config config; // the hyperparameters of the architecture (the blueprint)
    TransformerWeights weights; // the weights of the model
    RunState state; // buffers for the "wave" of activations in the forward pass
    // some more state needed to properly clean up the memory mapping (sigh)
    int fd; // file descriptor for memory mapping
    float* data; // memory mapped data pointer
    ssize_t file_size; // size of the checkpoint file in bytes
} Transformer;

void malloc_run_state(RunState* s, Config* p) {
    // we calloc instead of malloc to keep valgrind happy
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    s->x = calloc(p->dim, sizeof(float));
    s->xb = calloc(p->dim, sizeof(float));
    s->xb2 = calloc(p->dim, sizeof(float));
    s->hb = calloc(p->hidden_dim, sizeof(float));
    s->hb2 = calloc(p->hidden_dim, sizeof(float));
    s->q = calloc(p->dim, sizeof(float));
    s->key_cache = calloc(p->n_layers * p->seq_len * kv_dim, sizeof(float));
    s->value_cache = calloc(p->n_layers * p->seq_len * kv_dim, sizeof(float));
    s->att = calloc(p->n_heads * p->seq_len, sizeof(float));
    s->logits = calloc(p->vocab_size, sizeof(float));
    // ensure all mallocs went fine
    if (!s->x || !s->xb || !s->xb2 || !s->hb || !s->hb2 || !s->q
     || !s->key_cache || !s->value_cache || !s->att || !s->logits) {
        fprintf(stderr, "malloc failed!\n");
        exit(EXIT_FAILURE);
    }
}

void free_run_state(RunState* s) {
    free(s->x);
    free(s->xb);
    free(s->xb2);
    free(s->hb);
    free(s->hb2);
    free(s->q);
    free(s->att);
    free(s->logits);
    free(s->key_cache);
    free(s->value_cache);
}

void memory_map_weights(TransformerWeights *w, Config* p, float* ptr, int shared_weights) {
    int head_size = p->dim / p->n_heads;
    // make sure the multiplications below are done in 64bit to fit the parameter counts of 13B+ models
    unsigned long long n_layers = p->n_layers;
    w->token_embedding_table = ptr;
    ptr += p->vocab_size * p->dim;
    w->rms_att_weight = ptr;
    ptr += n_layers * p->dim;
    w->wq = ptr;
    ptr += n_layers * p->dim * (p->n_heads * head_size);
    w->wk = ptr;
    ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
    w->wv = ptr;
    ptr += n_layers * p->dim * (p->n_kv_heads * head_size);
    w->wo = ptr;
    ptr += n_layers * (p->n_heads * head_size) * p->dim;
    w->rms_ffn_weight = ptr;
    ptr += n_layers * p->dim;
    w->w1 = ptr;
    ptr += n_layers * p->dim * p->hidden_dim;
    w->w2 = ptr;
    ptr += n_layers * p->hidden_dim * p->dim;
    w->w3 = ptr;
    ptr += n_layers * p->dim * p->hidden_dim;
    w->rms_final_weight = ptr;
    ptr += p->dim;
    ptr += p->seq_len * head_size / 2; // skip what used to be freq_cis_real (for RoPE)
    ptr += p->seq_len * head_size / 2; // skip what used to be freq_cis_imag (for RoPE)
    w->wcls = shared_weights ? w->token_embedding_table : ptr;
}

/* ESP-IDF: the checkpoint is already in memory (a flash partition mapped by
 * llm_model.c, or a PSRAM copy of one), so there is nothing to read here --
 * parse the header and point the weights at what follows it.  Returns 0 on
 * success instead of calling exit(), so a bad blob is a log line rather than
 * a reboot loop. */
int read_checkpoint_blob(const void *blob, size_t blob_size,
                         Config* config, TransformerWeights* weights) {
    if (blob == NULL || blob_size < sizeof(Config)) {
        fprintf(stderr, "model blob too small: %u bytes\n", (unsigned)blob_size);
        return -1;
    }
    memcpy(config, blob, sizeof(Config));
    // negative vocab size is hacky way of signaling unshared weights. bit yikes.
    int shared_weights = config->vocab_size > 0 ? 1 : 0;
    config->vocab_size = abs(config->vocab_size);
    if (config->vocab_size <= 0 || config->dim <= 0 || config->n_layers <= 0) {
        fprintf(stderr, "model header looks wrong (vocab=%d dim=%d layers=%d)\n",
                config->vocab_size, config->dim, config->n_layers);
        return -1;
    }
    float* weights_ptr = (float*)((const char*)blob + sizeof(Config));
    memory_map_weights(weights, config, weights_ptr, shared_weights);
    return 0;
}

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

// ----------------------------------------------------------------------------
// neural net blocks; the dynamics of the Transformer

void rmsnorm(float* o, float* x, float* weight, int size) {
    // calculate sum of squares
    float ss = 0.0f;
    for (int j = 0; j < size; j++) {
        ss += x[j] * x[j];
    }
    ss /= size;
    ss += 1e-5f;
    ss = 1.0f / sqrtf(ss);
    // normalize and scale
    for (int j = 0; j < size; j++) {
        o[j] = weight[j] * (ss * x[j]);
    }
}

void softmax(float* x, int size) {
    // find max value (for numerical stability)
    float max_val = x[0];
    for (int i = 1; i < size; i++) {
        if (x[i] > max_val) {
            max_val = x[i];
        }
    }
    // exp and sum
    float sum = 0.0f;
    for (int i = 0; i < size; i++) {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }
    // normalize
    for (int i = 0; i < size; i++) {
        x[i] /= sum;
    }
}

void matmul(float* xout, float* x, float* w, int n, int d) {
    // W (d,n) @ x (n,) -> xout (d,)
    // by far the most amount of time is spent inside this little function
    int i;
    /* ESP-IDF: no OpenMP -- single core, see tools/port_llama2.py */
    for (i = 0; i < d; i++) {
        float val = 0.0f;
        for (int j = 0; j < n; j++) {
            val += w[i * n + j] * x[j];
        }
        xout[i] = val;
    }
}

float* forward(Transformer* transformer, int token, int pos) {

    // a few convenience variables
    Config* p = &transformer->config;
    TransformerWeights* w = &transformer->weights;
    RunState* s = &transformer->state;
    float *x = s->x;
    int dim = p->dim;
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    int kv_mul = p->n_heads / p->n_kv_heads; // integer multiplier of the kv sharing in multiquery
    int hidden_dim =  p->hidden_dim;
    int head_size = dim / p->n_heads;

    // copy the token embedding into x
    float* content_row = w->token_embedding_table + token * dim;
    memcpy(x, content_row, dim*sizeof(*x));

    // forward all the layers
    for(unsigned long long l = 0; l < p->n_layers; l++) {

        // attention rmsnorm
        rmsnorm(s->xb, x, w->rms_att_weight + l*dim, dim);

        // key and value point to the kv cache
        int loff = l * p->seq_len * kv_dim; // kv cache layer offset for convenience
        s->k = s->key_cache + loff + pos * kv_dim;
        s->v = s->value_cache + loff + pos * kv_dim;

        // qkv matmuls for this position
        matmul(s->q, s->xb, w->wq + l*dim*dim, dim, dim);
        matmul(s->k, s->xb, w->wk + l*dim*kv_dim, dim, kv_dim);
        matmul(s->v, s->xb, w->wv + l*dim*kv_dim, dim, kv_dim);

        // RoPE relative positional encoding: complex-valued rotate q and k in each head
        for (int i = 0; i < dim; i+=2) {
            int head_dim = i % head_size;
            float freq = 1.0f / powf(10000.0f, head_dim / (float)head_size);
            float val = pos * freq;
            float fcr = cosf(val);
            float fci = sinf(val);
            int rotn = i < kv_dim ? 2 : 1; // how many vectors? 2 = q & k, 1 = q only
            for (int v = 0; v < rotn; v++) {
                float* vec = v == 0 ? s->q : s->k; // the vector to rotate (query or key)
                float v0 = vec[i];
                float v1 = vec[i+1];
                vec[i]   = v0 * fcr - v1 * fci;
                vec[i+1] = v0 * fci + v1 * fcr;
            }
        }

        // multihead attention. iterate over all heads
        int h;
        /* ESP-IDF: no OpenMP (see the note in matmul) */
        for (h = 0; h < p->n_heads; h++) {
            // get the query vector for this head
            float* q = s->q + h * head_size;
            // attention scores for this head
            float* att = s->att + h * p->seq_len;
            // iterate over all timesteps, including the current one
            for (int t = 0; t <= pos; t++) {
                // get the key vector for this head and at this timestep
                float* k = s->key_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                // calculate the attention score as the dot product of q and k
                float score = 0.0f;
                for (int i = 0; i < head_size; i++) {
                    score += q[i] * k[i];
                }
                score /= sqrtf(head_size);
                // save the score to the attention buffer
                att[t] = score;
            }

            // softmax the scores to get attention weights, from 0..pos inclusively
            softmax(att, pos + 1);

            // weighted sum of the values, store back into xb
            float* xb = s->xb + h * head_size;
            memset(xb, 0, head_size * sizeof(float));
            for (int t = 0; t <= pos; t++) {
                // get the value vector for this head and at this timestep
                float* v = s->value_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                // get the attention weight for this timestep
                float a = att[t];
                // accumulate the weighted value into xb
                for (int i = 0; i < head_size; i++) {
                    xb[i] += a * v[i];
                }
            }
        }

        // final matmul to get the output of the attention
        matmul(s->xb2, s->xb, w->wo + l*dim*dim, dim, dim);

        // residual connection back into x
        for (int i = 0; i < dim; i++) {
            x[i] += s->xb2[i];
        }

        // ffn rmsnorm
        rmsnorm(s->xb, x, w->rms_ffn_weight + l*dim, dim);

        // Now for FFN in PyTorch we have: self.w2(F.silu(self.w1(x)) * self.w3(x))
        // first calculate self.w1(x) and self.w3(x)
        matmul(s->hb, s->xb, w->w1 + l*dim*hidden_dim, dim, hidden_dim);
        matmul(s->hb2, s->xb, w->w3 + l*dim*hidden_dim, dim, hidden_dim);

        // SwiGLU non-linearity
        for (int i = 0; i < hidden_dim; i++) {
            float val = s->hb[i];
            // silu(x)=x*σ(x), where σ(x) is the logistic sigmoid
            val *= (1.0f / (1.0f + expf(-val)));
            // elementwise multiply with w3(x)
            val *= s->hb2[i];
            s->hb[i] = val;
        }

        // final matmul to get the output of the ffn
        matmul(s->xb, s->hb, w->w2 + l*dim*hidden_dim, hidden_dim, dim);

        // residual connection
        for (int i = 0; i < dim; i++) {
            x[i] += s->xb[i];
        }
    }

    // final rmsnorm
    rmsnorm(x, x, w->rms_final_weight, dim);

    // classifier into logits
    matmul(s->logits, x, w->wcls, p->dim, p->vocab_size);
    return s->logits;
}

// ----------------------------------------------------------------------------
// The Byte Pair Encoding (BPE) Tokenizer that translates strings <-> tokens

typedef struct {
    char *str;
    int id;
} TokenIndex;

typedef struct {
    char** vocab;
    float* vocab_scores;
    TokenIndex *sorted_vocab;
    int vocab_size;
    unsigned int max_token_length;
    unsigned char byte_pieces[512]; // stores all single-byte strings
} Tokenizer;

int compare_tokens(const void *a, const void *b) {
    return strcmp(((TokenIndex*)a)->str, ((TokenIndex*)b)->str);
}

/* ESP-IDF: same byte layout as the file upstream reads, walked over a memory
 * blob instead.  Bounds-checked, and it returns -1 rather than exiting. */
int build_tokenizer_blob(Tokenizer* t, const void *blob, size_t blob_size, int vocab_size) {
    // i should have written the vocab_size into the tokenizer file... sigh
    t->vocab_size = vocab_size;
    t->vocab = (char**)malloc(vocab_size * sizeof(char*));
    t->vocab_scores = (float*)malloc(vocab_size * sizeof(float));
    t->sorted_vocab = NULL; // initialized lazily
    if (t->vocab == NULL || t->vocab_scores == NULL) {
        fprintf(stderr, "tokenizer: out of memory for %d entries\n", vocab_size);
        return -1;
    }
    for (int i = 0; i < 256; i++) {
        t->byte_pieces[i * 2] = (unsigned char)i;
        t->byte_pieces[i * 2 + 1] = '\0';
    }
    const char *p = (const char*)blob;
    const char *end = p + blob_size;
    if (p + sizeof(int) > end) { fprintf(stderr, "tokenizer blob too small\n"); return -1; }
    memcpy(&t->max_token_length, p, sizeof(int));
    p += sizeof(int);
    int len;
    for (int i = 0; i < vocab_size; i++) {
        if (p + sizeof(float) + sizeof(int) > end) {
            fprintf(stderr, "tokenizer truncated at entry %d\n", i);
            return -1;
        }
        memcpy(t->vocab_scores + i, p, sizeof(float));
        p += sizeof(float);
        memcpy(&len, p, sizeof(int));
        p += sizeof(int);
        if (len < 0 || p + len > end) {
            fprintf(stderr, "tokenizer entry %d has bad length %d\n", i, len);
            return -1;
        }
        t->vocab[i] = (char *)malloc(len + 1);
        if (t->vocab[i] == NULL) { fprintf(stderr, "tokenizer: oom at %d\n", i); return -1; }
        memcpy(t->vocab[i], p, len);
        p += len;
        t->vocab[i][len] = '\0'; // add the string terminating token
    }
    return 0;
}

void free_tokenizer(Tokenizer* t) {
    for (int i = 0; i < t->vocab_size; i++) { free(t->vocab[i]); }
    free(t->vocab);
    free(t->vocab_scores);
    free(t->sorted_vocab);
}

char* decode(Tokenizer* t, int prev_token, int token) {
    char *piece = t->vocab[token];
    // following BOS (1) token, sentencepiece decoder strips any leading whitespace (see PR #89)
    if (prev_token == 1 && piece[0] == ' ') { piece++; }
    // careful, some tokens designate raw bytes, and look like e.g. '<0x01>'
    // parse this and convert and return the actual byte
    unsigned char byte_val;
    if (sscanf(piece, "<0x%02hhX>", &byte_val) == 1) {
        piece = (char*)t->byte_pieces + byte_val * 2;
    }
    return piece;
}

void safe_printf(char *piece) {
    // piece might be a raw byte token, and we only want to print printable chars or whitespace
    // because some of the other bytes can be various control codes, backspace, etc.
    if (piece == NULL) { return; }
    if (piece[0] == '\0') { return; }
    if (piece[1] == '\0') {
        unsigned char byte_val = piece[0];
        if (!(isprint(byte_val) || isspace(byte_val))) {
            return; // bad byte, don't print it
        }
    }
    printf("%s", piece);
}

int str_lookup(char *str, TokenIndex *sorted_vocab, int vocab_size) {
    // efficiently find the perfect match for str in vocab, return its index or -1 if not found
    TokenIndex tok = { .str = str }; // acts as the key to search for
    TokenIndex *res = bsearch(&tok, sorted_vocab, vocab_size, sizeof(TokenIndex), compare_tokens);
    return res != NULL ? res->id : -1;
}

void encode(Tokenizer* t, char *text, int8_t bos, int8_t eos, int *tokens, int *n_tokens) {
    // encode the string text (input) into an upper-bound preallocated tokens[] array
    // bos != 0 means prepend the BOS token (=1), eos != 0 means append the EOS token (=2)
    if (text == NULL) { fprintf(stderr, "cannot encode NULL text\n"); exit(EXIT_FAILURE); }

    if (t->sorted_vocab == NULL) {
        // lazily malloc and sort the vocabulary
        t->sorted_vocab = malloc(t->vocab_size * sizeof(TokenIndex));
        for (int i = 0; i < t->vocab_size; i++) {
            t->sorted_vocab[i].str = t->vocab[i];
            t->sorted_vocab[i].id = i;
        }
        qsort(t->sorted_vocab, t->vocab_size, sizeof(TokenIndex), compare_tokens);
    }

    // create a temporary buffer that will store merge candidates of always two consecutive tokens
    // *2 for concat, +1 for null terminator +2 for UTF8 (in case max_token_length is 1)
    char* str_buffer = malloc((t->max_token_length*2 +1 +2) * sizeof(char));
    size_t str_len = 0;

    // start at 0 tokens
    *n_tokens = 0;

    // add optional BOS (=1) token, if desired
    if (bos) tokens[(*n_tokens)++] = 1;

    // add_dummy_prefix is true by default
    // so prepend a dummy prefix token to the input string, but only if text != ""
    // TODO: pretty sure this isn't correct in the general case but I don't have the
    // energy to read more of the sentencepiece code to figure out what it's doing
    if (text[0] != '\0') {
        int dummy_prefix = str_lookup(" ", t->sorted_vocab, t->vocab_size);
        tokens[(*n_tokens)++] = dummy_prefix;
    }

    // Okay UTF-8 time. This will get messy. Here is the reference from Wikipedia:
    // Code point ↔ UTF-8 conversion
    // First code point	Last code point	Byte 1	Byte 2	Byte 3	Byte 4
    // U+0000	U+007F	    0xxxxxxx
    // U+0080	U+07FF	    110xxxxx	10xxxxxx
    // U+0800	U+FFFF	    1110xxxx	10xxxxxx	10xxxxxx
    // U+10000	U+10FFFF    11110xxx	10xxxxxx	10xxxxxx	10xxxxxx

    // process the raw (UTF-8) byte sequence of the input string
    for (char *c = text; *c != '\0'; c++) {

        // reset buffer if the current byte is ASCII or a leading byte
        // 0xC0 is 11000000, so (*c & 0xC0) keeps the first 2 bits and zeros the rest
        // 0x80 is 10000000
        // in UTF-8, all continuation bytes start with "10" in first two bits
        // so in English this is: "if this byte is not a continuation byte"
        if ((*c & 0xC0) != 0x80) {
            // this byte must be either a leading byte (11...) or an ASCII char (0x...)
            // => reset our location, as we're starting a new UTF-8 codepoint
            str_len = 0;
        }

        // append the current byte to the buffer
        str_buffer[str_len++] = *c; // ++ is post-increment, incremented after this line
        str_buffer[str_len] = '\0';

        // while the next character is a continuation byte, continue appending
        // but if there are too many of them, just stop to avoid overruning str_buffer size.
        if ((*(c+1) & 0xC0) == 0x80 && str_len < 4) {
            continue;
        }

        // ok c+1 is not a continuation byte, so we've read in a full codepoint
        int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);

        if (id != -1) {
            // we found this codepoint in vocab, add it as a token
            tokens[(*n_tokens)++] = id;
        } else {
            // byte_fallback encoding: just encode each byte as a token
            // +3 is here because the first 3 vocab elements are <unk>, <s>, </s>
            // so the individual bytes only start at index 3
            for (int i=0; i < str_len; i++) {
                tokens[(*n_tokens)++] = (unsigned char)str_buffer[i] + 3;
            }
        }
        str_len = 0; // protect against a sequence of stray UTF8 continuation bytes
    }

    // merge the best consecutive pair each iteration, according the scores in vocab_scores
    while (1) {
        float best_score = -1e10;
        int best_id = -1;
        int best_idx = -1;

        for (int i=0; i < (*n_tokens-1); i++) {
            // check if we can merge the pair (tokens[i], tokens[i+1])
            sprintf(str_buffer, "%s%s", t->vocab[tokens[i]], t->vocab[tokens[i+1]]);
            int id = str_lookup(str_buffer, t->sorted_vocab, t->vocab_size);
            if (id != -1 && t->vocab_scores[id] > best_score) {
                // this merge pair exists in vocab! record its score and position
                best_score = t->vocab_scores[id];
                best_id = id;
                best_idx = i;
            }
        }

        if (best_idx == -1) {
            break; // we couldn't find any more pairs to merge, so we're done
        }

        // merge the consecutive pair (best_idx, best_idx+1) into new token best_id
        tokens[best_idx] = best_id;
        // delete token at position best_idx+1, shift the entire sequence back 1
        for (int i = best_idx+1; i < (*n_tokens-1); i++) {
            tokens[i] = tokens[i+1];
        }
        (*n_tokens)--; // token length decreased
    }

    // add optional EOS (=2) token, if desired
    if (eos) tokens[(*n_tokens)++] = 2;

    free(str_buffer);
}

// ----------------------------------------------------------------------------
// The Sampler, which takes logits and returns a sampled token
// sampling can be done in a few ways: greedy argmax, sampling, top-p sampling

typedef struct {
    float prob;
    int index;
} ProbIndex; // struct used when sorting probabilities during top-p sampling

typedef struct {
    int vocab_size;
    ProbIndex* probindex; // buffer used in top-p sampling
    float temperature;
    float topp;
    unsigned long long rng_state;
} Sampler;

int sample_argmax(float* probabilities, int n) {
    // return the index that has the highest probability
    int max_i = 0;
    float max_p = probabilities[0];
    for (int i = 1; i < n; i++) {
        if (probabilities[i] > max_p) {
            max_i = i;
            max_p = probabilities[i];
        }
    }
    return max_i;
}

int sample_mult(float* probabilities, int n, float coin) {
    // sample index from probabilities (they must sum to 1!)
    // coin is a random number in [0, 1), usually from random_f32()
    float cdf = 0.0f;
    for (int i = 0; i < n; i++) {
        cdf += probabilities[i];
        if (coin < cdf) {
            return i;
        }
    }
    return n - 1; // in case of rounding errors
}

int compare(const void* a, const void* b) {
    ProbIndex* a_ = (ProbIndex*) a;
    ProbIndex* b_ = (ProbIndex*) b;
    if (a_->prob > b_->prob) return -1;
    if (a_->prob < b_->prob) return 1;
    return 0;
}

int sample_topp(float* probabilities, int n, float topp, ProbIndex* probindex, float coin) {
    // top-p sampling (or "nucleus sampling") samples from the smallest set of
    // tokens that exceed probability topp. This way we never sample tokens that
    // have very low probabilities and are less likely to go "off the rails".
    // coin is a random number in [0, 1), usually from random_f32()

    int n0 = 0;
    // quicksort indices in descending order of probabilities
    // values smaller than (1 - topp) / (n - 1) cannot be part of the result
    // so for efficiency we crop these out as candidates before sorting
    const float cutoff = (1.0f - topp) / (n - 1);
    for (int i = 0; i < n; i++) {
        if (probabilities[i] >= cutoff) {
            probindex[n0].index = i;
            probindex[n0].prob = probabilities[i];
            n0++;
        }
    }
    qsort(probindex, n0, sizeof(ProbIndex), compare);

    // truncate the list where cumulative probability exceeds topp
    float cumulative_prob = 0.0f;
    int last_idx = n0 - 1; // in case of rounding errors consider all elements
    for (int i = 0; i < n0; i++) {
        cumulative_prob += probindex[i].prob;
        if (cumulative_prob > topp) {
            last_idx = i;
            break; // we've exceeded topp by including last_idx
        }
    }

    // sample from the truncated list
    float r = coin * cumulative_prob;
    float cdf = 0.0f;
    for (int i = 0; i <= last_idx; i++) {
        cdf += probindex[i].prob;
        if (r < cdf) {
            return probindex[i].index;
        }
    }
    return probindex[last_idx].index; // in case of rounding errors
}

void build_sampler(Sampler* sampler, int vocab_size, float temperature, float topp, unsigned long long rng_seed) {
    sampler->vocab_size = vocab_size;
    sampler->temperature = temperature;
    sampler->topp = topp;
    sampler->rng_state = rng_seed;
    // buffer only used with nucleus sampling; may not need but it's ~small
    sampler->probindex = malloc(sampler->vocab_size * sizeof(ProbIndex));
}

void free_sampler(Sampler* sampler) {
    free(sampler->probindex);
}

unsigned int random_u32(unsigned long long *state) {
    // xorshift rng: https://en.wikipedia.org/wiki/Xorshift#xorshift.2A
    *state ^= *state >> 12;
    *state ^= *state << 25;
    *state ^= *state >> 27;
    return (*state * 0x2545F4914F6CDD1Dull) >> 32;
}
float random_f32(unsigned long long *state) { // random float32 in [0,1)
    return (random_u32(state) >> 8) / 16777216.0f;
}

int sample(Sampler* sampler, float* logits) {
    // sample the token given the logits and some hyperparameters
    int next;
    if (sampler->temperature == 0.0f) {
        // greedy argmax sampling: take the token with the highest probability
        next = sample_argmax(logits, sampler->vocab_size);
    } else {
        // apply the temperature to the logits
        for (int q=0; q<sampler->vocab_size; q++) { logits[q] /= sampler->temperature; }
        // apply softmax to the logits to get the probabilities for next token
        softmax(logits, sampler->vocab_size);
        // flip a (float) coin (this is our source of entropy for sampling)
        float coin = random_f32(&sampler->rng_state);
        // we sample from this distribution to get the next token
        if (sampler->topp <= 0 || sampler->topp >= 1) {
            // simply sample from the predicted probability distribution
            next = sample_mult(logits, sampler->vocab_size, coin);
        } else {
            // top-p (nucleus) sampling, clamping the least likely tokens to zero
            next = sample_topp(logits, sampler->vocab_size, sampler->topp, sampler->probindex, coin);
        }
    }
    return next;
}

// ----------------------------------------------------------------------------
// utilities: time

long time_in_ms() {
    // return time in milliseconds, for benchmarking the model speed
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    return time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

// ----------------------------------------------------------------------------
// generation loop

void generate(Transformer *transformer, Tokenizer *tokenizer, Sampler *sampler, char *prompt, int steps) {
    char *empty_prompt = "";
    if (prompt == NULL) { prompt = empty_prompt; }

    // encode the (string) prompt into tokens sequence
    int num_prompt_tokens = 0;
    int* prompt_tokens = (int*)malloc((strlen(prompt)+3) * sizeof(int)); // +3 for '\0', ?BOS, ?EOS
    encode(tokenizer, prompt, 1, 0, prompt_tokens, &num_prompt_tokens);
    if (num_prompt_tokens < 1) {
        fprintf(stderr, "something is wrong, expected at least 1 prompt token\n");
        exit(EXIT_FAILURE);
    }

    // start the main loop
    long start = 0;  // used to time our code, only initialized after first iteration
    int next;        // will store the next token in the sequence
    int token = prompt_tokens[0]; // kick off with the first token in the prompt
    int pos = 0;     // position in the sequence
    while (pos < steps) {

        // forward the transformer to get logits for the next token
        float* logits = forward(transformer, token, pos);

        // advance the state machine
        if (pos < num_prompt_tokens - 1) {
            // if we are still processing the input prompt, force the next prompt token
            next = prompt_tokens[pos + 1];
        } else {
            // otherwise sample the next token from the logits
            next = sample(sampler, logits);
        }
        pos++;

        // data-dependent terminating condition: the BOS (=1) token delimits sequences
        if (next == 1) { break; }

        // print the token as string, decode it with the Tokenizer object
        char* piece = decode(tokenizer, token, next);
        llm_emit(piece); // hands the piece to the caller instead of stdout
        token = next;

        // init the timer here because the first iteration can be slower
        if (start == 0) { start = time_in_ms(); }
    }
    llm_emit("\n");

    // report achieved tok/s (pos-1 because the timer starts after first iteration)
    if (pos > 1) {
        long end = time_in_ms();
        double tok_per_sec = (pos-1) / (double)(end-start)*1000;
        fprintf(stderr, "achieved tok/s: %f\n", tok_per_sec);
        llm_note_speed(tok_per_sec);
    }

    free(prompt_tokens);
}



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
