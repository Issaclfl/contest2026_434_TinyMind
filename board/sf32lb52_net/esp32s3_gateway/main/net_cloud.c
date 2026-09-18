/*
 * Cloud LLM route -- the part that turns the gateway from "a router that also
 * runs a model" into one answerable question: does this request go up or stay
 * here?
 *
 * The policy is deliberately about connectivity, not about pretending to
 * understand the question: a small model cannot judge whether a prompt is
 * "hard", but the gateway can know whether the internet is there.  Model
 * names choose explicitly ("local" always stays), everything else tries the
 * cloud and degrades to the local model when the uplink is gone or the cloud
 * call fails.  The board keeps pointing its agent at one address and never
 * learns that two different models may answer.
 *
 * The key lives in sdkconfig (gitignored), entered by tools/set_cloud.py in
 * the operator's own terminal.  It is sent to the cloud and never logged,
 * served, or echoed here.
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include "net_cloud.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "espllm.h"
#include "net_ppp.h"
#include "net_wifi.h"

#include "sdkconfig.h"

static const char *TAG = "gw_cloud";

/* The cloud can think for tens of seconds; the board's agent waits 60 s per
 * attempt and retries, so anything slower than this would not be useful even
 * if it came back.  The connect itself fails in low single-digit seconds when
 * the uplink is gone, which is where the fast fallback comes from. */
#define CLOUD_TIMEOUT_MS CONFIG_GATEWAY_CLOUD_TIMEOUT_MS
#define CLOUD_MAX_RESP CONFIG_GATEWAY_CLOUD_MAX_RESP
#define MODEL_MAX 64

typedef struct {
    uint32_t cloud_ok;
    uint32_t cloud_fail;    /* tried the cloud, failed, fell back */
    uint32_t offline_local; /* no uplink, did not even try */
    uint32_t local_direct;  /* explicitly asked for the local model */
    uint32_t no_key_local;  /* cloud not configured at all */
} cloud_stats_t;

static cloud_stats_t s_stats;
static int64_t s_last_us;
static char s_last_prompt[121];
static char s_last_route[24];

static void note_prompt(const char *body);

bool net_cloud_configured(void)
{
    return CONFIG_GATEWAY_CLOUD_API_KEY[0] != '\0';
}

/* ------------------------------------------------------------ uplink test -- */

static bool uplink_ready(void)
{
    esp_netif_t *sta = net_wifi_netif();
    if (sta == NULL || !esp_netif_is_netif_up(sta)) {
        return false;
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    /* An association without a default route cannot reach a name on the
     * internet; treat the gateway address as the proxy for "the uplink is
     * really up". */
    return info.gw.addr != 0;
}

/* ----------------------------------------------------- the HTTP callback -- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
} resp_accum_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }
    resp_accum_t *acc = evt->user_data;
    size_t need = acc->len + (size_t)evt->data_len + 1;
    if (need > CLOUD_MAX_RESP) {
        acc->overflow = true;
        return ESP_OK;   /* keep the connection harmless; we will fail it below */
    }
    if (need > acc->cap) {
        size_t cap = acc->cap * 2;
        if (cap < need) {
            cap = need;
        }
        /* The response is not moved by any DMA engine; PSRAM is fine and
         * spares the internal heap the WiFi stack is also living in. */
        char *grown = heap_caps_realloc(acc->buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (grown == NULL) {
            acc->overflow = true;
            return ESP_OK;
        }
        acc->buf = grown;
        acc->cap = cap;
    }
    memcpy(acc->buf + acc->len, evt->data, (size_t)evt->data_len);
    acc->len += (size_t)evt->data_len;
    acc->buf[acc->len] = '\0';
    return ESP_OK;
}

/* --------------------------------------------------------- the cloud call -- */

/* Rewrites "model":"<anything>" to the configured cloud model so the board's
 * name for its route ("auto") never leaks into a cloud that wants a real one.
 * Returns a heap copy of the body, or NULL. */
static char *rewrite_model(const char *body, size_t body_len)
{
    char found[MODEL_MAX];
    const char *needle = "\"model\":\"";
    const char *p = strstr(body, needle);
    if (p == NULL) {
        ESP_LOGW(TAG, "request has no model field; sending it as-is");
        char *copy = heap_caps_malloc(body_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy != NULL) {
            memcpy(copy, body, body_len + 1);
        }
        return copy;
    }
    p += strlen(needle);
    const char *end = strchr(p, '"');
    if (end == NULL) {
        ESP_LOGW(TAG, "request model field is malformed; sending it as-is");
        char *copy = heap_caps_malloc(body_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy != NULL) {
            memcpy(copy, body, body_len + 1);
        }
        return copy;
    }
    size_t old_len = (size_t)(end - p);
    if (old_len >= sizeof(found)) {
        old_len = sizeof(found) - 1;
    }
    memcpy(found, p, old_len);
    found[old_len] = '\0';

    const char *new_model = CONFIG_GATEWAY_CLOUD_MODEL;
    size_t prefix = (size_t)(p - body);
    size_t new_len = body_len - old_len + strlen(new_model);
    char *out = heap_caps_malloc(new_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, body, prefix);
    memcpy(out + prefix, new_model, strlen(new_model));
    memcpy(out + prefix + strlen(new_model), body + prefix + old_len,
           body_len - prefix - old_len);
    out[new_len] = '\0';
    if (strcmp(found, new_model) != 0) {
        ESP_LOGI(TAG, "model \"%s\" -> \"%s\"", found, new_model);
    }
    return out;
}

static bool cloud_ask(const char *body, size_t body_len, char **response)
{
    char *out_body = rewrite_model(body, body_len);
    if (out_body == NULL) {
        return false;
    }

    resp_accum_t acc = { 0 };
    acc.cap = 4096;
    acc.buf = heap_caps_malloc(acc.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (acc.buf == NULL) {
        free(out_body);
        return false;
    }
    acc.buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = CONFIG_GATEWAY_CLOUD_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CLOUD_TIMEOUT_MS,
        .event_handler = on_http_event,
        .user_data = &acc,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(acc.buf);
        free(out_body);
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", "Bearer " CONFIG_GATEWAY_CLOUD_API_KEY);
    esp_http_client_set_post_field(client, out_body, (int)body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(out_body);

    bool ok = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cloud call failed: %s", esp_err_to_name(err));
    } else if (status != 200) {
        ESP_LOGW(TAG, "cloud answered HTTP %d", status);
    } else if (acc.overflow || acc.buf == NULL || acc.len == 0) {
        ESP_LOGW(TAG, "cloud answer incomplete (%u bytes%s)",
                 (unsigned)acc.len, acc.overflow ? ", over the cap" : "");
    } else {
        ok = true;
    }

    if (ok) {
        *response = acc.buf;
    } else {
        free(acc.buf);
    }
    return ok;
}

/* ---------------------------------------------------------- the routing -- */

static esp_err_t route(const char *body, size_t body_len, char **response)
{
    int64_t t0 = esp_timer_get_time();
    note_prompt(body);

    char model[MODEL_MAX];
    if (!espllm_json_field(body, "model", model, sizeof(model))) {
        strlcpy(model, "(none)", sizeof(model));
    }

    /* Explicit local: the board asked for this device's own model by name. */
    if (strcmp(model, "local") == 0 || strcmp(model, CONFIG_ESPLM_MODEL_ID) == 0) {
        s_stats.local_direct++;
        strlcpy(s_last_route, "local (asked)", sizeof(s_last_route));
        return ESP_ERR_NOT_FOUND;   /* fall through to the local model */
    }

    if (!net_cloud_configured()) {
        s_stats.no_key_local++;
        strlcpy(s_last_route, "local (no key)", sizeof(s_last_route));
        return ESP_ERR_NOT_FOUND;
    }
    if (!uplink_ready()) {
        s_stats.offline_local++;
        ESP_LOGW(TAG, "no uplink -- the local model answers (offline mode)");
        strlcpy(s_last_route, "local (offline)", sizeof(s_last_route));
        return ESP_ERR_NOT_FOUND;
    }

    bool ok = cloud_ask(body, body_len, response);
    if (!ok) {
        s_stats.cloud_fail++;
        ESP_LOGW(TAG, "falling back to the local model");
        strlcpy(s_last_route, "local (fallback)", sizeof(s_last_route));
        return ESP_ERR_NOT_FOUND;
    }

    s_stats.cloud_ok++;
    s_last_us = esp_timer_get_time() - t0;
    strlcpy(s_last_route, "cloud", sizeof(s_last_route));
    ESP_LOGI(TAG, "cloud answered in %.1f s", s_last_us / 1e6);
    return ESP_OK;
}

/* --------------------------------------------------------- status lines -- */

static int status_lines(char *out, size_t out_size)
{
    size_t used = 0;
    used += (size_t)snprintf(out + used, out_size - used,
                             "route : %s, %u cloud ok / %u fail->local / "
                             "%u offline->local / %u direct local / %u no-key\n",
                             s_last_route,
                             (unsigned)s_stats.cloud_ok, (unsigned)s_stats.cloud_fail,
                             (unsigned)s_stats.offline_local, (unsigned)s_stats.local_direct,
                             (unsigned)s_stats.no_key_local);
    if (s_last_prompt[0] != '\0') {
        used += (size_t)snprintf(out + used, out_size - used,
                                 "asked : \"%s\"\n", s_last_prompt);
    }

    bool up = uplink_ready();
    used += (size_t)snprintf(out + used, out_size - used,
                             "uplink: %s\nboard : %s\n",
                             up ? "Wi-Fi up" : "Wi-Fi down -- serving the local model",
                             net_ppp_is_up() ? "PPP session up (10.0.0.2)"
                                             : "waiting for the board to dial in");
    if (up) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            used += (size_t)snprintf(out + used, out_size - used,
                                     "        RSSI %d dBm\n", ap.rssi);
        }
    }
    return (int)used;
}

/* The router sees every body, so it also records the newest question for the
 * dashboard.  Runs in the httpd task; a torn line on a status page costs
 * nothing, so there is no locking. */
static void note_prompt(const char *body)
{
    char content[160];
    if (espllm_json_field(body, "content", content, sizeof(content)) &&
        content[0] != '\0') {
        size_t n = strlen(content);
        if (n > 80) {
            /* keep the tail: the question is at the end */
            memmove(content, content + n - 80, 81);
            n = 80;
        }
        content[n] = '\0';
        strlcpy(s_last_prompt, content, sizeof(s_last_prompt));
    }
}

void net_cloud_start(void)
{
    espllm_set_router(route);
    espllm_set_status_provider(status_lines);

    if (net_cloud_configured()) {
        ESP_LOGI(TAG, "cloud route on: %s (model %s) -- uplink requests go there, "
                      "the local model covers offline",
                 CONFIG_GATEWAY_CLOUD_URL, CONFIG_GATEWAY_CLOUD_MODEL);
        ESP_LOGI(TAG, "the key is in sdkconfig only (gitignored); it is never logged");
    } else {
        ESP_LOGI(TAG, "no cloud key configured -- the local model answers everything "
                      "(tools/set_cloud.py sets one in sdkconfig)");
    }
}
