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
#include "cmd_exec.h"
#include "net_miio.h"
#include "net_clf.h"
#include "cJSON.h"
#include "net_ppp.h"
#include "net_wifi.h"

#include "sdkconfig.h"

static const char *TAG = "gw_cloud";

/* 请求里写这个名字（`set_llm <地址> cmd local`）就走到"命令模型 + 动作执行"那条路：
 * 本地小模型把口令翻成动作 JSON，由 cmd_exec_run 在本板上真的做掉。 */
#define CMD_MODEL_NAME "cmd"

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
    uint32_t cmd_run;       /* ran the command model and executed an action */
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
 * Returns a heap copy of the body and its length, or NULL.  The length is an
 * out-parameter because the rewrite changes it: sending the original length
 * with the rewritten body truncates the JSON and the cloud answers 400. */
static char *rewrite_model(const char *body, size_t body_len, size_t *out_len)
{
    char found[MODEL_MAX];
    const char *needle = "\"model\":\"";
    const char *p = strstr(body, needle);
    if (p == NULL) {
        /* No model field: an OpenAI-compatible client that leaves it out used to
         * get a 400 from the cloud ("Unsupported model unknown-model"), which
         * looked like a cloud failure and silently degraded to the local model --
         * a question about the world came back as an action JSON.  The cloud
         * requires the field, so put ours in. */
        const char *brace = strchr(body, '{');
        const char *new_model = CONFIG_GATEWAY_CLOUD_MODEL;
        if (brace == NULL) {
            ESP_LOGW(TAG, "request has no JSON object; sending it as-is");
            char *copy = heap_caps_malloc(body_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (copy != NULL) {
                memcpy(copy, body, body_len + 1);
                *out_len = body_len;
            }
            return copy;
        }
        size_t prefix = (size_t)(brace - body) + 1;
        size_t add = strlen("\"model\":\"") + strlen(new_model) + strlen("\",") ;
        size_t new_len = body_len + add;
        char *out = heap_caps_malloc(new_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (out == NULL) {
            return NULL;
        }
        memcpy(out, body, prefix);
        int n = snprintf(out + prefix, new_len + 1 - prefix, "\"model\":\"%s\",", new_model);
        memcpy(out + prefix + (size_t)n, body + prefix, body_len - prefix);
        out[new_len] = '\0';
        *out_len = new_len;
        ESP_LOGI(TAG, "request had no model field; added \"%s\" (%u -> %u B body)",
                 new_model, (unsigned)body_len, (unsigned)new_len);
        return out;
    }
    p += strlen(needle);
    const char *end = strchr(p, '"');
    if (end == NULL) {
        ESP_LOGW(TAG, "request model field is malformed; sending it as-is");
        char *copy = heap_caps_malloc(body_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (copy != NULL) {
            memcpy(copy, body, body_len + 1);
            *out_len = body_len;
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
    *out_len = new_len;
    if (strcmp(found, new_model) != 0) {
        ESP_LOGI(TAG, "model \"%s\" -> \"%s\" (%u -> %u B body)",
                 found, new_model, (unsigned)body_len, (unsigned)new_len);
    }
    return out;
}

/* 一次 POST：body 进，响应攒进 acc。返回 HTTP 状态码，-1 表示压根没连上。 */
static int cloud_post(const char *body, size_t body_len, resp_accum_t *acc)
{
    esp_http_client_config_t cfg = {
        .url = CONFIG_GATEWAY_CLOUD_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CLOUD_TIMEOUT_MS,
        .event_handler = on_http_event,
        .user_data = acc,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return -1;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", "Bearer " CONFIG_GATEWAY_CLOUD_API_KEY);
    esp_http_client_set_post_field(client, body, (int)body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cloud call failed: %s", esp_err_to_name(err));
        return -1;
    }
    return status;
}

static bool accum_init(resp_accum_t *acc)
{
    acc->cap = 4096;
    acc->len = 0;
    acc->overflow = false;
    acc->buf = heap_caps_malloc(acc->cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (acc->buf != NULL) {
        acc->buf[0] = '\0';
    }
    return acc->buf != NULL;
}

static bool cloud_ask(const char *body, size_t body_len, char **response)
{
    size_t out_len = body_len;
    char *out_body = rewrite_model(body, body_len, &out_len);
    if (out_body == NULL) {
        return false;
    }

    resp_accum_t acc = { 0 };
    if (!accum_init(&acc)) {
        free(out_body);
        return false;
    }

    int status = cloud_post(out_body, out_len, &acc);
    free(out_body);

    bool ok = false;
    if (status < 0) {
        /* already logged by cloud_post */
    } else if (status != 200) {
        /* The cloud's error text is the fastest way to see what it disliked
         * about the request; it is a diagnostics message, not user content. */
        ESP_LOGW(TAG, "cloud answered HTTP %d: %.160s", status,
                 acc.buf != NULL ? acc.buf : "(no body)");
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

    /* The command model: the board asks for it by name ("cmd"), and what comes
     * back is not an essay but an action -- cmd_exec_run runs it on this board
     * and answers with the JSON it executed.  This is the "let the model drive
     * the hardware" route; the client only needs set_llm ... cmd local. */
    if (strcmp(model, CMD_MODEL_NAME) == 0) {
        char text[192] = { 0 };
        char answer[768];
        if (!espllm_json_field_last(body, "content", text, sizeof(text))) {
            snprintf(answer, sizeof(answer), "no command text in the request");
            ESP_LOGW(TAG, "cmd: no content field");
        } else {
            ESP_LOGI(TAG, "cmd: %s", text);
            /* 先问本地分类器（完全离线）：它只判断“这句话是哪个动作”。
             * 命中米家动作就交给 miIO 客户端执行，再把结果翻成一句中文；
             * 不是米家动作则照旧走原来的规则执行器（英文口令那套不受影响）。 */
            const char *label = net_clf_classify(text);
            cJSON *doc = (label != NULL) ? cJSON_Parse(label) : NULL;
            cJSON *jact = doc ? cJSON_GetObjectItem(doc, "action") : NULL;
            if (jact != NULL && cJSON_IsString(jact) && strcmp(jact->valuestring, "miio") == 0) {
                const char *dev = "lamp";
                const char *op = "";
                int value = 0;
                cJSON *j = cJSON_GetObjectItem(doc, "device");
                if (j != NULL && cJSON_IsString(j)) { dev = j->valuestring; }
                j = cJSON_GetObjectItem(doc, "op");
                if (j != NULL && cJSON_IsString(j)) { op = j->valuestring; }
                j = cJSON_GetObjectItem(doc, "value");
                if (j != NULL && cJSON_IsNumber(j)) { value = (int)j->valuedouble; }
                char raw[512];
                char line[64];
                char zh[192];
                snprintf(line, sizeof(line), "%s %s %d", dev, op, value);
                net_miio_run(dev, op, value, NULL, raw, sizeof(raw));
                net_miio_zh(line, raw, zh, sizeof(zh));
                snprintf(answer, sizeof(answer), "%s", zh);
                ESP_LOGI(TAG, "cmd->clf->miio: %s => %s", label, answer);
            } else if (jact != NULL && cJSON_IsString(jact) &&
                       strcmp(jact->valuestring, "led") == 0) {
                /* 板载 RGB 灯（"打开RGB" / "红灯闪三下"）：分类器已经把颜色、
                 * 特效、次数说清楚了，直接执行，不再让生成式小模型去猜 ——
                 * 那个模型是英文语料训的，中文进去只会瞎猜（这正是"打开RGB"
                 * 一度被当成开台灯的原因）。 */
                char color[24] = { 0 };
                char effect[16] = { 0 };
                int times = 1;
                char raw[256];
                cJSON *j = cJSON_GetObjectItem(doc, "color");
                if (j != NULL && cJSON_IsString(j)) {
                    snprintf(color, sizeof(color), "%s", j->valuestring);
                }
                j = cJSON_GetObjectItem(doc, "effect");
                if (j != NULL && cJSON_IsString(j)) {
                    snprintf(effect, sizeof(effect), "%s", j->valuestring);
                }
                j = cJSON_GetObjectItem(doc, "times");
                if (j != NULL && cJSON_IsNumber(j)) {
                    times = (int)j->valuedouble;
                }
                cmd_exec_led(color, effect, times, raw, sizeof(raw), answer, sizeof(answer));
                ESP_LOGI(TAG, "cmd->clf->led: %s => %s", label, answer);
            } else if (cmd_exec_run(text, answer, sizeof(answer)) != 0) {
                ESP_LOGW(TAG, "cmd failed: %s", answer);
            }
            if (doc != NULL) { cJSON_Delete(doc); }
        }
        s_stats.cmd_run++;
        s_last_us = esp_timer_get_time() - t0;
        strlcpy(s_last_route, "local (command)", sizeof(s_last_route));

        char *esc = malloc(strlen(answer) * 2 + 1);
        char *resp = malloc(strlen(answer) * 2 + 512);
        if (esc == NULL || resp == NULL) {
            free(esc);
            free(resp);
            return ESP_ERR_NO_MEM;
        }
        size_t o = 0;
        for (const char *p = answer; *p != '\0' && o + 2 < strlen(answer) * 2 + 1; p++) {
            if (*p == '"' || *p == '\\') { esc[o++] = '\\'; }
            if (*p == '\n') { esc[o++] = '\\'; esc[o++] = 'n'; continue; }
            esc[o++] = *p;
        }
        esc[o] = '\0';
        snprintf(resp, strlen(answer) * 2 + 512,
                 "{\"id\":\"chatcmpl-cmd\",\"object\":\"chat.completion\","
                 "\"created\":%lld,\"model\":\"" CMD_MODEL_NAME "\","
                 "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
                 "\"content\":\"%s\"},\"finish_reason\":\"stop\"}],"
                 "\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":0,\"total_tokens\":0}}",
                 (long long)(esp_timer_get_time() / 1000000), esc);
        free(esc);
        *response = resp;
        ESP_LOGI(TAG, "cmd answered in %.1f s: %s", (esp_timer_get_time() - t0) / 1e6, answer);
        return ESP_OK;
    }

    /* 米家设备（局域网直控）：出现 model:"miio" 时**不经过模型**，把确定性文本
     * 直接交给 net_miio_run_text。先用它把"协议链路"单独验通，再让本地小模型
     * 去生成同样的动作 —— 两件事分开验，出问题才知道是哪一层。 */
    if (strcmp(model, "miio") == 0) {
        char text[192] = { 0 };
        char answer[768];
        if (!espllm_json_field_last(body, "content", text, sizeof(text))) {
            snprintf(answer, sizeof(answer), "no command text in the request");
        } else {
            ESP_LOGI(TAG, "miio: %s", text);
            if (net_miio_run_text(text, answer, sizeof(answer)) != 0) {
                ESP_LOGW(TAG, "miio failed: %s", answer);
            }
        }
        s_stats.cmd_run++;
        s_last_us = esp_timer_get_time() - t0;
        strlcpy(s_last_route, "local (miio)", sizeof(s_last_route));

        /* 转义后拼成 OpenAI 形状的响应。**只有一个 %s，也只有一个实参** ——
         * 上一版把 %lld 和 %s 混着写、又把 " 漏成了裸引号，编译器直接拒了。 */
        /* 板子要念的是中文；机器可读的 JSON 另挂在 miio_result 字段里。 */
        char zh[192];
        net_miio_zh(text, answer, zh, sizeof(zh));

        char *esc = malloc(strlen(zh) * 2 + 1);
        char *resp = malloc(strlen(zh) * 2 + strlen(answer) + 640);
        if (esc == NULL || resp == NULL) {
            free(esc);
            free(resp);
            return ESP_ERR_NO_MEM;
        }
        size_t o = 0;
        for (const char *q = zh; *q != '\0'; q++) {
            if (*q == '"' || *q == '\\') {
                esc[o++] = '\\';
            } else if (*q == '\n') {
                esc[o++] = '\\';
                esc[o++] = 'n';
                continue;
            }
            esc[o++] = *q;
        }
        esc[o] = '\0';
        snprintf(resp, strlen(zh) * 2 + strlen(answer) + 640,
                 "{\"id\":\"chatcmpl-miio\",\"object\":\"chat.completion\",\"created\":0,"
                 "\"model\":\"miio\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"ass"
                 "istant\",\"content\":\"%s\"},\"finish_reason\":\"stop\"}],\"usage\":{\"p"
                 "rompt_tokens\":0,\"completion_tokens\":0,\"total_tokens\":0}}",
                 esc);
        free(esc);
        *response = resp;
        ESP_LOGI(TAG, "miio: %s", answer);
        return ESP_OK;
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

/* 给命令模型的 status 动作（以及任何想复用网关状态的地方）用：和追加到
 * GET /status 后面的那段是同一条实现。 */
int net_cloud_status(char *out, size_t out_size)
{
    return status_lines(out, out_size);
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

/* ------------------------------------------------- 一句问答（ask 动作）------ */

/* JSON 字符串转义：够用就行（引号、反斜杠、控制字符）。 */
static size_t json_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const char *p = src; *p != '\0' && o + 8 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c == '\n') {
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if (c == '\r') {
            dst[o++] = '\\';
            dst[o++] = 'r';
        } else if (c == '\t') {
            dst[o++] = '\\';
            dst[o++] = 't';
        } else if (c < 0x20) {
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return o;
}

/* 700 而不是 200：mimo 是思考型模型，token 给少了它把额度全花在"想"上，正文是空的
 * （实测过，见证据文档 §八的补记）。 */
#define ASK_MAX_TOKENS 700

int net_cloud_ask_text(const char *text, char *out, size_t out_size)
{
    if (text == NULL || text[0] == '\0') {
        snprintf(out, out_size, "nothing to ask");
        return -1;
    }
    if (!net_cloud_configured()) {
        snprintf(out, out_size, "no cloud key on this gateway (tools/set_cloud.py)");
        return -1;
    }
    if (!uplink_ready()) {
        snprintf(out, out_size, "no uplink -- answering a question needs the internet");
        return -1;
    }

    size_t esc_cap = strlen(text) * 6 + 16;
    char *esc = heap_caps_malloc(esc_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (esc == NULL) {
        snprintf(out, out_size, "out of memory");
        return -1;
    }
    (void)json_escape(text, esc, esc_cap);

    size_t body_cap = strlen(esc) + 256;
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == NULL) {
        free(esc);
        snprintf(out, out_size, "out of memory");
        return -1;
    }
    int n = snprintf(body, body_cap,
                     "{\"model\":\"%s\",\"max_tokens\":%d,\"messages\":"
                     "[{\"role\":\"user\",\"content\":\"%s\"}]}",
                     CONFIG_GATEWAY_CLOUD_MODEL, ASK_MAX_TOKENS, esc);
    free(esc);
    if (n <= 0 || (size_t)n >= body_cap) {
        free(body);
        snprintf(out, out_size, "that question is too long");
        return -1;
    }

    resp_accum_t acc = { 0 };
    if (!accum_init(&acc)) {
        free(body);
        snprintf(out, out_size, "out of memory");
        return -1;
    }
    int status = cloud_post(body, (size_t)n, &acc);
    free(body);

    if (status < 0) {
        free(acc.buf);
        snprintf(out, out_size, "could not reach the cloud");
        return -1;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "ask: HTTP %d: %.160s", status, acc.buf != NULL ? acc.buf : "(no body)");
        free(acc.buf);
        snprintf(out, out_size, "cloud answered HTTP %d", status);
        return -1;
    }
    /* 取**最后**一个 content：思考型模型的回答正文在 reasoning_content 之后，
     * 而 reasoning_content 里的正文也可能出现同名字样（内部只有全文扫描）。 */
    bool got = espllm_json_field_last(acc.buf, "content", out, out_size);
    size_t len = got ? strlen(out) : 0;
    free(acc.buf);
    if (!got) {
        snprintf(out, out_size, "cloud answered without content");
        return -1;
    }
    if (len == 0) {
        snprintf(out, out_size,
                 "the cloud only reasoned and never answered (max_tokens too small?)");
        return -1;
    }
    ESP_LOGI(TAG, "ask: %u chars back", (unsigned)len);
    return 0;
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
