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
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "espllm.http";

#define BODY_MAX_BYTES 4096
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

/* --------------------------------------------------------------- handlers -- */

static esp_err_t h_status(httpd_req_t *req)
{
    char body[640];
    const espllm_config_t *cfg = espllm_config();
    if (cfg == NULL) {
        snprintf(body, sizeof(body),
                 "espllm -- local model node on ESP32-S3\n\n"
                 "state : model not loaded\n");
    } else {
        snprintf(body, sizeof(body),
                 "espllm -- local model node on ESP32-S3\n\n"
                 "model : " CONFIG_ESPLM_MODEL_ID ", %d layers, dim %d, hidden %d,\n"
                 "        %d heads (%d kv), vocab %d, seq_len %d\n"
                 "last  : %.1f tok/s, %u chars%s\n"
                 "api   : GET /v1/models, POST /v1/chat/completions\n",
                 cfg->n_layers, cfg->dim, cfg->hidden_dim,
                 cfg->n_heads, cfg->n_kv_heads, cfg->vocab_size, cfg->seq_len,
                 espllm_tok_per_sec(), (unsigned)espllm_text_len(),
                 espllm_text_truncated() ? " (truncated)" : "");
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, body);
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
    int got = httpd_req_recv(req, body, req->content_len);
    if (got <= 0) {
        free(body);
        return send_error(req, "400 Bad Request", "could not read the body");
    }
    body[got] = '\0';

    /* The user's turn is the last message, so search from the end.  "prompt"
     * is accepted too, for the non-chat completion shape. */
    const char *value = json_value(body, "content", true);
    if (value == NULL) {
        value = json_value(body, "prompt", true);
    }
    char prompt[PROMPT_MAX_BYTES];
    if (value == NULL || !json_string(&value, prompt, sizeof(prompt)) || prompt[0] == '\0') {
        free(body);
        return send_error(req, "400 Bad Request", "no usable content/prompt string");
    }

    int max_tokens = CONFIG_ESPLM_HTTP_MAX_TOKENS;
    const char *mt = json_value(body, "max_tokens", true);
    if (mt != NULL && *mt >= '0' && *mt <= '9') {
        max_tokens = atoi(mt);
    }
    if (max_tokens < 1) {
        max_tokens = 1;
    }
    if (max_tokens > CONFIG_ESPLM_TOKENS_LIMIT) {
        max_tokens = CONFIG_ESPLM_TOKENS_LIMIT;
    }
    ESP_LOGI(TAG, "prompt (%u chars): %s", (unsigned)strlen(prompt), prompt);
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

esp_err_t espllm_http_start(uint16_t port)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    /* Generation runs on this task (see espllm_generate), so its stack has to
     * carry the engine's frames, not just the HTTP parser's. */
    config.stack_size = CONFIG_ESPLM_HTTP_TASK_STACK;
    config.max_uri_handlers = 8;
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

    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_status },
        { .uri = "/v1/models", .method = HTTP_GET, .handler = h_models },
        { .uri = "/v1/chat/completions", .method = HTTP_POST, .handler = h_chat },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "could not register %s: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "openai-compatible endpoint listening on port %u "
                  "(GET /, GET /v1/models, POST /v1/chat/completions)", (unsigned)port);
    return ESP_OK;
}
