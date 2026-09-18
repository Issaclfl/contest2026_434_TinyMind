/*
 * 语音入口：说一句英文口令，灯就亮。
 *
 * 链路（每一段都单独验证过，见 docs/ESP32-S3网关台架实测证据.md §十一）：
 *
 *     POST /voice 的音频
 *       -> 云端 mimo 的 chat 接口（把音频当 input_audio 内容块发过去）
 *       -> 它只做转写，返回文本
 *       -> 本地命令模型（就是我们自己训、自己量化的那个）把文本翻成动作 JSON
 *       -> cmd_exec_run() 在本板执行
 *
 * 为什么转写放云端、意图放本地：云端那个模型一听就懂自由说法，但它是思考型模型，
 * 让它直接输出动作 JSON 会绕远（实测 800 token 都在自说自话）；而"几种说法对应
 * 哪个动作"正是我们本地 0.26M 模型的强项（留出集 43/46），还不用把动作词汇表
 * 交给外面。所以两边各干自己擅长的：**云负责听，端负责选**。
 *
 * 代价说明白：语音这条腿需要联网。断了网还能用打字的口令（走 cmd 那条路），
 * 那一条完全是本地的。
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voice_exec.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "espllm.h"
#include "cmd_exec.h"
#include "net_cloud.h"

static const char *TAG = "gw_voice";

/* 转写是短任务，给 200 token 足够；音频上限就是 espllm 那边的 512 KB。
 *
 * 注意这里**不带文字提示**：实测给 ASR 模型配一段文字会被拒
 * （"ASR request must not include text parts; text prompt is injected by the
 * gateway"），只发音频它才 1.6 秒返回；换通用 chat 模型也能用，但它会先
 * 自言自语 7~8 秒才肯转写。 */
#define ASR_MAX_TOKENS 200

/* ---------------------------------------------------------------- 小工具 -- */

static size_t b64_len(size_t n) { return 4 * ((n + 2) / 3); }

/* 找 WAV 的 data 块；不是 RIFF 就当裸 PCM（我们自己约定的 16k/单声道/16bit）。 */
static const char *pcm_start(const char *audio, size_t len, size_t *pcm_len)
{
    if (len > 12 && memcmp(audio, "RIFF", 4) == 0 && memcmp(audio + 8, "WAVE", 4) == 0) {
        size_t pos = 12;
        while (pos + 8 <= len) {
            const char *id = audio + pos;
            uint32_t sz = (uint8_t)audio[pos + 4] | ((uint32_t)(uint8_t)audio[pos + 5] << 8) |
                          ((uint32_t)(uint8_t)audio[pos + 6] << 16) |
                          ((uint32_t)(uint8_t)audio[pos + 7] << 24);
            if (memcmp(id, "data", 4) == 0) {
                size_t avail = len - pos - 8;
                *pcm_len = sz < avail ? sz : avail;
                return audio + pos + 8;
            }
            pos += 8 + sz + (sz & 1);
        }
        *pcm_len = 0;
        return NULL;
    }
    *pcm_len = len;
    return audio;
}

static void json_escape_into(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    for (const char *p = src; *p != '\0' && o + 3 < dst_size; p++) {
        if (*p == '"' || *p == '\\') {
            dst[o++] = '\\';
            dst[o++] = *p;
        } else if (*p == '\n') {
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if ((unsigned char)*p >= 0x20) {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}

/* ------------------------------------------------------------------- ASR -- */

/* 把一段 WAV 交给云端的 mimo，返回它转写出的文本（写进 out）。 */
static bool asr_transcribe(const char *wav, size_t wav_len, char *out, size_t out_size)
{
    const char *key = CONFIG_GATEWAY_CLOUD_API_KEY;
    if (key[0] == '\0') {
        snprintf(out, out_size, "no cloud key configured");
        return false;
    }

    size_t b64_size = b64_len(wav_len) + 4;
    char *b64 = heap_caps_malloc(b64_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t body_cap = b64_size + 1024;
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *resp = heap_caps_malloc(CONFIG_GATEWAY_CLOUD_MAX_RESP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (b64 == NULL || body == NULL || resp == NULL) {
        free(b64);
        free(body);
        free(resp);
        snprintf(out, out_size, "out of memory for %u B of audio", (unsigned)wav_len);
        return false;
    }

    extern int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                                     const unsigned char *src, size_t slen);
    size_t b64_used = 0;
    if (mbedtls_base64_encode((unsigned char *)b64, b64_size, &b64_used,
                              (const unsigned char *)wav, wav_len) != 0) {
        free(b64);
        free(body);
        free(resp);
        snprintf(out, out_size, "base64 failed");
        return false;
    }
    b64[b64_used] = '\0';

    int n = snprintf(body, body_cap,
                     "{\"model\":\"%s\",\"max_tokens\":%d,\"messages\":[{\"role\":\"user\","
                     "\"content\":[{\"type\":\"input_audio\","
                     "\"input_audio\":{\"data\":\"%s\",\"format\":\"wav\"}}]}]}",
                     CONFIG_GATEWAY_ASR_MODEL, ASR_MAX_TOKENS, b64);
    free(b64);
    if (n <= 0 || (size_t)n >= body_cap) {
        free(body);
        free(resp);
        snprintf(out, out_size, "request body too large");
        return false;
    }

    esp_http_client_config_t cfg = {
        .url = CONFIG_GATEWAY_CLOUD_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = CONFIG_GATEWAY_CLOUD_TIMEOUT_MS,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        free(body);
        free(resp);
        snprintf(out, out_size, "http client init failed");
        return false;
    }
    esp_http_client_set_method(c, HTTP_METHOD_POST);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", key);
    esp_http_client_set_header(c, "Authorization", auth);
    esp_http_client_set_post_field(c, body, n);

    size_t got = 0;
    int status = 0;
    esp_err_t err = esp_http_client_open(c, n);
    if (err == ESP_OK) {
        esp_http_client_write(c, body, n);
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        int r;
        while (got + 1 < CONFIG_GATEWAY_CLOUD_MAX_RESP &&
               (r = esp_http_client_read(c, resp + got, CONFIG_GATEWAY_CLOUD_MAX_RESP - got - 1)) > 0) {
            got += (size_t)r;
        }
        resp[got] = '\0';
    } else {
        snprintf(out, out_size, "upload failed: %s", esp_err_to_name(err));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(body);

    if (err != ESP_OK) {
        free(resp);
        return false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "asr HTTP %d: %.160s", status, resp);
        free(resp);
        snprintf(out, out_size, "asr failed (HTTP %d)", status);
        return false;
    }
    if (!espllm_json_field(resp, "content", out, out_size)) {
        ESP_LOGW(TAG, "asr answered without content: %.160s", resp);
        free(resp);
        snprintf(out, out_size, "no transcript in the answer");
        return false;
    }
    free(resp);
    ESP_LOGI(TAG, "transcript: \"%s\"", out);
    return out[0] != '\0';
}

/* ------------------------------------------------------------ 请求入口 -- */

static esp_err_t voice_handle(const char *audio, size_t audio_len, char **response)
{
    size_t pcm_len = 0;
    const char *pcm = pcm_start(audio, audio_len, &pcm_len);
    if (pcm == NULL || pcm_len < 1600) {          /* < 50 ms：当没录到东西 */
        *response = strdup("{\"error\":\"too short to contain speech\"}");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "%u B uploaded, %u B of pcm (%u ms at 16 kHz mono)",
             (unsigned)audio_len, (unsigned)pcm_len, (unsigned)(pcm_len / 32));

    /* 云端要一个完整容器：原本是 WAV 就整段送，裸 PCM 就给它补个 WAV 头。 */
    static const unsigned char kHdr[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E','f','m','t',' ', 16,0,0,0,
        1,0,1,0, 0x80,0x3e,0,0, 0x00,0x7d,0,0, 2,0,16,0, 'd','a','t','a', 0,0,0,0 };
    char *wav = NULL;
    size_t wav_len = 0;
    if (memcmp(audio, "RIFF", 4) == 0) {
        wav = (char *)audio;
        wav_len = audio_len;
    } else {
        wav_len = pcm_len + 44;
        wav = heap_caps_malloc(wav_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (wav == NULL) {
            *response = strdup("{\"error\":\"out of memory\"}");
            return ESP_OK;
        }
        memcpy(wav, kHdr, 44);
        uint32_t sz = (uint32_t)pcm_len;
        memcpy(wav + 4, &(uint32_t){ sz + 36 }, 4);
        memcpy(wav + 40, &sz, 4);
        memcpy(wav + 44, pcm, pcm_len);
    }

    char transcript[256] = { 0 };
    bool ok = asr_transcribe(wav, wav_len, transcript, sizeof(transcript));
    if (wav != audio) {
        free(wav);
    }
    if (!ok) {
        char esc[512];
        json_escape_into(transcript, esc, sizeof(esc));
        char *out = malloc(strlen(esc) + 64);
        if (out != NULL) {
            snprintf(out, strlen(esc) + 64, "{\"error\":\"%s\"}", esc);
            *response = out;
        }
        return ESP_OK;
    }

    /* 听懂之后交给本地命令模型：动作的词汇表不出这块板子。 */
    char answer[640];
    int rc = cmd_exec_run(transcript, answer, sizeof(answer));

    char esc[512];
    json_escape_into(transcript, esc, sizeof(esc));
    size_t cap = strlen(esc) + strlen(answer) + 128;
    char *out = malloc(cap);
    if (out == NULL) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(out, cap, "{\"transcript\":\"%s\",\"ok\":%s,\"result\":%s}",
             esc, rc == 0 ? "true" : "false", answer);
    *response = out;
    ESP_LOGI(TAG, "voice -> %s", answer);
    return ESP_OK;
}

void voice_start(void)
{
    espllm_set_voice(voice_handle, NULL);   /* NULL = 用组件里那个内置页面 */
    ESP_LOGI(TAG, "voice route on: say an English command at http://<gateway>/talk");
}
