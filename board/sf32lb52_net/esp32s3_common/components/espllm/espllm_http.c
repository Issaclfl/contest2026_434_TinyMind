/*
 * espllm -- OpenAI-compatible HTTP endpoints.
 *
 * Deliberately no JSON library: the requests this device has to understand are
 * small and come from clients we control (the board's agent, curl, a browser),
 * so a couple of hundred bytes of string scanning beats pulling in a parser.
 * The trade-off is that a key appearing inside a string value can confuse the
 * scanner; json_value(..., from_end=true) takes the LAST match, which is what
 * makes "the user's message" the right one to pick out of "messages".
 *
 * The endpoints are reachable on any interface that has an address: the Wi-Fi
 * station, or the PPP link once the board dials in.
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include "espllm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "espllm.http";

/* Request bodies are not small: the board's agent sends its system prompt and
 * the whole tool catalogue, measured at ~15.5 KB.  The limit is about not
 * letting an arbitrary client make us allocate without bound, not about
 * keeping requests tiny. */
#define BODY_MAX_BYTES 32768
/* 语音请求体要大得多：4 秒 16 kHz 单声道是 128 KB，浏览器再 base64 编码也就 171 KB。 */
#define VOICE_MAX_BYTES (512 * 1024)
#define PROMPT_MAX_BYTES 512

/* ---------------------------------------------------------------- JSON in -- */

/* Points at the value that follows "key": (whitespace skipped).  Returns the
 * last match when from_end, else the first. */
static const char *json_value(const char *json, const char *key, bool from_end)
{
    size_t klen = strlen(key);
    const char *found = NULL;

    for (const char *p = json; (p = strchr(p, '"')) != NULL; p++) {
        if (strncmp(p + 1, key, klen) != 0 || p[1 + klen] != '"') {
            continue;
        }
        const char *q = p + 2 + klen;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
            q++;
        }
        if (*q != ':') {
            continue;
        }
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
            q++;
        }
        found = q;
        if (!from_end) {
            return found;
        }
        p = q;   /* keep scanning for a later one */
    }
    return found;
}

/* Same search as json_value, but only accepts a value that is a JSON string.
 *
 * Needed for "content": the board's agent sends its tool catalogue, and one of
 * those tools takes a parameter called "content" (write_file).  A plain
 * last-match search therefore lands on a tool *object* rather than on the
 * user's message, and the request is rejected for having no usable prompt.
 * Requiring a quote keeps the real message. */
static const char *json_string_value(const char *json, const char *key, bool from_end)
{
    size_t klen = strlen(key);
    const char *found = NULL;

    for (const char *p = json; (p = strchr(p, '"')) != NULL; p++) {
        if (strncmp(p + 1, key, klen) != 0 || p[1 + klen] != '"') {
            continue;
        }
        const char *q = p + 2 + klen;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
            q++;
        }
        if (*q != ':') {
            continue;
        }
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
            q++;
        }
        if (*q != '"') {
            continue;   /* an object, a number, null -- not the user's text */
        }
        found = q;
        if (!from_end) {
            return found;
        }
        p = q;
    }
    return found;
}

static size_t utf8_encode(long cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

/* Decodes the JSON string at *pp (which must point at the opening quote).
 * \uXXXX is turned back into UTF-8, because the tokenizers are byte-oriented
 * and a prompt full of '?' would quietly change the question. */
static bool json_string(const char **pp, char *out, size_t out_size)
{
    const char *p = *pp;
    if (*p != '"') {
        return false;
    }
    p++;
    size_t o = 0;
    while (*p != '\0' && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            char e = *p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'u': {
                char hex[5] = { 0, 0, 0, 0, 0 };
                for (int i = 0; i < 4 && p[i] != '\0'; i++) {
                    hex[i] = p[i];
                }
                p += 4;
                char enc[4];
                size_t n = utf8_encode(strtol(hex, NULL, 16), enc);
                if (o + n >= out_size) {
                    return false;
                }
                memcpy(out + o, enc, n);
                o += n;
                continue;
            }
            default: c = (unsigned char)e; break;
            }
        }
        if (o + 1 >= out_size) {
            return false;
        }
        out[o++] = (char)c;
    }
    if (*p != '"') {
        return false;
    }
    *pp = p + 1;
    out[o] = '\0';
    return true;
}

/* --------------------------------------------------------------- JSON out -- */

static size_t json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        const char *rep = NULL;
        char buf[8];
        switch (*p) {
        case '"': rep = "\\\""; break;
        case '\\': rep = "\\\\"; break;
        case '\n': rep = "\\n"; break;
        case '\r': rep = "\\r"; break;
        case '\t': rep = "\\t"; break;
        default:
            if (*p < 0x20) {
                snprintf(buf, sizeof(buf), "\\u%04x", *p);
                rep = buf;
            }
            break;
        }
        if (rep != NULL) {
            size_t n = strlen(rep);
            if (o + n + 1 > dst_size) {
                break;
            }
            memcpy(dst + o, rep, n);
            o += n;
        } else {
            if (o + 1 + 1 > dst_size) {
                break;
            }
            dst[o++] = (char)*p;
        }
    }
    dst[o] = '\0';
    return o;
}

/* ---------------------------------------------------------------- helpers -- */

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":{\"message\":\"%s\"}}", msg);
    return send_json(req, status, body);
}

/* ------------------------------------------------------------- the hooks -- */

static espllm_router_fn s_router;
static espllm_status_fn s_status;
static espllm_voice_fn s_voice;
static const char *s_voice_page;

void espllm_set_router(espllm_router_fn router)
{
    s_router = router;
}

void espllm_set_status_provider(espllm_status_fn provider)
{
    s_status = provider;
}

void espllm_set_voice(espllm_voice_fn handler, const char *page)
{
    s_voice = handler;
    s_voice_page = page;
}

bool espllm_json_field(const char *json, const char *key, char *out, size_t out_size)
{
    const char *value = json_string_value(json, key, false);
    if (value == NULL) {
        return false;
    }
    return json_string(&value, out, out_size);
}

bool espllm_json_field_last(const char *json, const char *key, char *out, size_t out_size)
{
    const char *value = json_string_value(json, key, true);
    if (value == NULL) {
        return false;
    }
    return json_string(&value, out, out_size);
}

/* --------------------------------------------------------------- handlers -- */

static esp_err_t h_status(httpd_req_t *req)
{
    char body[1024];
    const espllm_config_t *cfg = espllm_config();
    if (cfg == NULL) {
        snprintf(body, sizeof(body),
                 "espllm -- local model node on ESP32-S3\n\n"
                 "state : model not loaded\n");
    } else {
        snprintf(body, sizeof(body),
                 "model : " CONFIG_ESPLM_MODEL_ID ", %d layers, dim %d, hidden %d,\n"
                 "        %d heads (%d kv), vocab %d, seq_len %d\n"
                 "weights: %s\n"
                 "last  : %.1f tok/s, %u chars%s\n",
                 cfg->n_layers, cfg->dim, cfg->hidden_dim,
                 cfg->n_heads, cfg->n_kv_heads, cfg->vocab_size, cfg->seq_len,
                 espllm_weight_format(),
                 espllm_tok_per_sec(), (unsigned)espllm_text_len(),
                 espllm_text_truncated() ? " (truncated)" : "");
    }
    if (s_status != NULL) {
        size_t used = strlen(body);
        s_status(body + used, sizeof(body) - used);
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, body);
}

/* The dashboard a browser (or a phone on the same Wi-Fi) sees.  A styled <pre>
 * block, not charts: this is a device with a serial console, and the page is
 * the same picture the console draws -- refreshed every few seconds so the
 * routing counters move while someone is watching. */
static esp_err_t h_dashboard(httpd_req_t *req)
{
    char text[1024];
    char page[2200];
    const espllm_config_t *cfg = espllm_config();
    if (cfg == NULL) {
        snprintf(text, sizeof(text), "state : model not loaded\n");
    } else {
        snprintf(text, sizeof(text),
                 "model : " CONFIG_ESPLM_MODEL_ID ", %d layers, dim %d\n"
                 "weights: %s\n"
                 "last  : %.1f tok/s, %u chars%s\n",
                 cfg->n_layers, cfg->dim, espllm_weight_format(),
                 espllm_tok_per_sec(), (unsigned)espllm_text_len(),
                 espllm_text_truncated() ? " (truncated)" : "");
    }
    if (s_status != NULL) {
        size_t used = strlen(text);
        s_status(text + used, sizeof(text) - used);
    }

    snprintf(page, sizeof(page),
             "<!doctype html><html><head><meta charset=\"utf-8\">"
             "<meta http-equiv=\"refresh\" content=\"3\">"
             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
             "<title>TinyMind edge gateway</title><style>"
             "body{background:#101418;color:#d8e0e8;font-family:monospace;"
             "display:flex;justify-content:center;margin:0;padding:24px 8px}"
             "pre{white-space:pre-wrap;font-size:15px;line-height:1.5;max-width:640px}"
             "h1{font-size:16px;color:#7fd0a0;font-weight:normal;margin:0 0 12px}"
             "</style></head><body><pre>"
             "<h1>TinyMind-Audio edge gateway (ESP32-S3)</h1>"
             "%s"
             "api   : GET /v1/models, POST /v1/chat/completions</pre></body></html>",
             text);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, page);
}

static esp_err_t h_models(httpd_req_t *req)
{
    char body[256];
    snprintf(body, sizeof(body),
             "{\"object\":\"list\",\"data\":[{\"id\":\"" CONFIG_ESPLM_MODEL_ID "\","
             "\"object\":\"model\",\"created\":%lld,\"owned_by\":\"tinymind\"}]}",
             (long long)time(NULL));
    return send_json(req, "200 OK", body);
}

static esp_err_t h_chat(httpd_req_t *req)
{
    if (!espllm_ready()) {
        return send_error(req, "503 Service Unavailable", "model not loaded");
    }
    if (req->content_len <= 0 || req->content_len > BODY_MAX_BYTES) {
        return send_error(req, "413 Payload Too Large", "body missing or too large");
    }

    char *body = malloc(req->content_len + 1);
    if (body == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    /* A single recv returns only what has arrived so far -- over the PPP link
     * the agent's ~15 KB request lands in many 1500-byte segments, so keep
     * reading until the announced Content-Length is actually in hand.  The
     * truncated first read is exactly what made every string value end without
     * its closing quote. */
    size_t total = 0;
    while (total < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + total, req->content_len - total);
        if (n <= 0) {
            free(body);
            return send_error(req, "400 Bad Request", "could not read the body");
        }
        total += (size_t)n;
    }
    body[total] = '\0';

    /* A registered router gets first say: it may answer from somewhere else
     * (the gateway uses this for its cloud route) and anything else falls
     * through to the local model below. */
    if (s_router != NULL) {
        char *routed = NULL;
        esp_err_t rerr = s_router(body, total, &routed);
        if (rerr == ESP_OK && routed != NULL) {
            esp_err_t sent = send_json(req, "200 OK", routed);
            free(routed);
            return sent;
        }
    }

    /* The user's turn is the last message, so search from the end.  "prompt"
     * is accepted too, for the non-chat completion shape.  A client may put
     * far more in "content" than this model can use, so an over-long prompt is
     * cut to its tail -- the question is at the end -- rather than rejected. */
    const char *value = json_string_value(body, "content", true);
    if (value == NULL) {
        value = json_string_value(body, "prompt", true);
    }
    size_t body_len = total;
    char prompt[PROMPT_MAX_BYTES];
    if (value != NULL) {
        /* Decode into a buffer the size of the request (decoded text is never
         * longer than its encoding), then keep the tail when the model's
         * window cannot hold it all.  Cutting mid-UTF-8 is acceptable here:
         * the tokenizers are byte-oriented, so a split sequence costs one odd
         * token instead of the whole question. */
        char *decoded = malloc(body_len + 1);
        if (decoded == NULL) {
            free(body);
            return send_error(req, "500 Internal Server Error", "out of memory");
        }
        prompt[0] = '\0';
        if (json_string(&value, decoded, body_len + 1)) {
            size_t n = strlen(decoded);
            const char *src = decoded;
            if (n >= sizeof(prompt)) {
                src = decoded + (n - (sizeof(prompt) - 1));
                ESP_LOGW(TAG, "prompt is %u chars, keeping the last %u",
                         (unsigned)n, (unsigned)(sizeof(prompt) - 1));
            }
            memcpy(prompt, src, strlen(src) + 1);
        }
        free(decoded);
    } else {
        prompt[0] = '\0';
    }
    if (prompt[0] == '\0') {
        free(body);
        return send_error(req, "400 Bad Request", "no usable content/prompt string");
    }

    int max_tokens = CONFIG_ESPLM_HTTP_MAX_TOKENS;
    const char *mt = json_value(body, "max_tokens", true);
    if (mt == NULL) {
        mt = json_value(body, "max_completion_tokens", true);   /* the agent's name for it */
    }
    if (mt != NULL && *mt >= '0' && *mt <= '9') {
        max_tokens = atoi(mt);
    }
    if (max_tokens < 1) {
        max_tokens = 1;
    }
    if (max_tokens > CONFIG_ESPLM_TOKENS_LIMIT) {
        max_tokens = CONFIG_ESPLM_TOKENS_LIMIT;
    }
    /* An OpenAI client may pin the sampling temperature, and 0 (greedy) is what
     * makes two weight formats comparable.  A request that says nothing gets the
     * configured default, so one caller pinning 0 does not change the next
     * caller's answers. */
    float temperature = CONFIG_ESPLM_TEMPERATURE / 100.0f;
    const char *tv = json_value(body, "temperature", true);
    if (tv != NULL && ((*tv >= '0' && *tv <= '9') || *tv == '.')) {
        temperature = strtof(tv, NULL);
        if (temperature < 0.0f) { temperature = 0.0f; }
        if (temperature > 2.0f) { temperature = 2.0f; }
    }
    espllm_set_temperature(temperature);
    ESP_LOGI(TAG, "request: %u B body, prompt (%u chars): %s",
             (unsigned)body_len, (unsigned)strlen(prompt), prompt);
    ESP_LOGI(TAG, "sampling: temperature %.2f, max_tokens %d",
             (double)temperature, max_tokens);
    free(body);

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = espllm_generate(prompt, max_tokens);
    int64_t took_us = esp_timer_get_time() - t0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "generation failed: %s", esp_err_to_name(err));
        return send_error(req, "500 Internal Server Error", "generation failed");
    }

    const char *text = espllm_text();
    size_t text_len = espllm_text_len();
    size_t esc_size = text_len * 2 + 1;
    char *esc = malloc(esc_size);
    if (esc == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    json_escape(text, esc, esc_size);

    size_t cap = text_len * 2 + 512;
    char *resp = malloc(cap);
    if (resp == NULL) {
        free(esc);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    int n = snprintf(resp, cap,
                     "{\"id\":\"chatcmpl-espllm\",\"object\":\"chat.completion\","
                     "\"created\":%lld,\"model\":\"" CONFIG_ESPLM_MODEL_ID "\","
                     "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
                     "\"content\":\"%s\"},\"finish_reason\":\"stop\"}],"
                     "\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":%u,"
                     "\"total_tokens\":%u}}",
                     (long long)time(NULL), esc, (unsigned)text_len, (unsigned)text_len);
    free(esc);

    if (n < 0 || (size_t)n >= cap) {
        free(resp);
        return send_error(req, "500 Internal Server Error", "response too large");
    }
    esp_err_t sent = send_json(req, "200 OK", resp);
    free(resp);

    ESP_LOGI(TAG, "answered: %u chars in %.1f s (%.1f tok/s)",
             (unsigned)text_len, took_us / 1e6, espllm_tok_per_sec());
    return sent;
}

/* 「按住说话」这个页面**不由组件提供**：页面文案、录音交互，以及"浏览器只在
 * 安全上下文里给麦克风"这件事都是应用侧的内容（网关那台机器上是
 * main/voice_exec.c 里的 kVoicePage）。组件只负责把 s_voice_page 原样发出去，
 * 没设页面时如实说没有，不去假装有一个能用的页面。 */
static esp_err_t h_talk(httpd_req_t *req)
{
    if (s_voice_page == NULL) {
        return send_error(req, "503 Service Unavailable", "no voice page configured");
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, s_voice_page);
}

static esp_err_t h_voice(httpd_req_t *req)
{
    if (s_voice == NULL) {
        return send_error(req, "503 Service Unavailable", "voice is not configured");
    }
    if (req->content_len <= 0 || req->content_len > VOICE_MAX_BYTES) {
        return send_error(req, "413 Payload Too Large", "audio missing or too large");
    }
    char *audio = malloc((size_t)req->content_len);
    if (audio == NULL) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    size_t total = 0;
    while (total < (size_t)req->content_len) {
        int n = httpd_req_recv(req, audio + total, req->content_len - total);
        if (n <= 0) {
            free(audio);
            return send_error(req, "400 Bad Request", "could not read the audio");
        }
        total += (size_t)n;
    }
    ESP_LOGI(TAG, "voice: %u B of audio", (unsigned)total);

    char *response = NULL;
    esp_err_t err = s_voice(audio, total, &response);
    free(audio);
    if (err != ESP_OK || response == NULL) {
        free(response);
        return send_error(req, "500 Internal Server Error", "voice handler failed");
    }
    esp_err_t sent = send_json(req, "200 OK", response);
    free(response);
    return sent;
}

static const httpd_uri_t kUris[] = {
    { .uri = "/", .method = HTTP_GET, .handler = h_dashboard },
    { .uri = "/status", .method = HTTP_GET, .handler = h_status },
    { .uri = "/talk", .method = HTTP_GET, .handler = h_talk },
    { .uri = "/voice", .method = HTTP_POST, .handler = h_voice },
    { .uri = "/v1/models", .method = HTTP_GET, .handler = h_models },
    { .uri = "/v1/chat/completions", .method = HTTP_POST, .handler = h_chat },
};

static esp_err_t register_uris(httpd_handle_t server)
{
    for (size_t i = 0; i < sizeof(kUris) / sizeof(kUris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &kUris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not register %s: %s", kUris[i].uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t espllm_http_start(uint16_t port)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    /* Generation runs on this task (see espllm_generate), so its stack has to
     * carry the engine's frames, not just the HTTP parser's. */
    config.stack_size = CONFIG_ESPLM_HTTP_TASK_STACK;
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    /* A full answer can take tens of seconds to produce; the timeout that
     * matters is the one on sending the response. */
    config.send_wait_timeout = 60;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed on port %u: %s", (unsigned)port, esp_err_to_name(err));
        return err;
    }
    err = register_uris(server);
    if (err != ESP_OK) {
        httpd_stop(server);
        return err;
    }

    ESP_LOGI(TAG, "openai-compatible endpoint listening on port %u "
                  "(GET /, GET /v1/models, POST /v1/chat/completions)", (unsigned)port);
    return ESP_OK;
}

esp_err_t espllm_http_start_secure(uint16_t port, const uint8_t *cert, size_t cert_len,
                                   const uint8_t *key, size_t key_len)
{
    if (cert == NULL || key == NULL || cert_len == 0 || key_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    config.servercert = cert;
    config.servercert_len = cert_len;
    config.prvtkey_pem = key;
    config.prvtkey_len = key_len;
    config.port_secure = port;
    config.httpd.max_uri_handlers = sizeof(kUris) / sizeof(kUris[0]);
    /* Same reason as above: /v1/chat/completions runs generation on this task. */
    config.httpd.stack_size = CONFIG_ESPLM_HTTP_TASK_STACK;
    config.httpd.lru_purge_enable = true;
    config.httpd.recv_wait_timeout = 10;
    config.httpd.send_wait_timeout = 60;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_ssl_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start failed on port %u: %s", (unsigned)port, esp_err_to_name(err));
        return err;
    }
    err = register_uris(server);
    if (err != ESP_OK) {
        httpd_ssl_stop(server);
        return err;
    }

    ESP_LOGI(TAG, "the same routes are also on TLS port %u (a browser only hands out "
                  "the microphone to a secure origin)", (unsigned)port);
    return ESP_OK;
}
