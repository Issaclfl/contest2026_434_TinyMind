/*
 * 命令执行器：把本地小模型吐出的动作 JSON 变成真的动作。
 *
 * 这是"让模型控制硬件"的最后一环。模型（0.26M 参数，stories260K 微调而来，
 * 见 esp32s3_common/tools/train_cmd_model.py）只负责一件事：把一句英文口令翻成
 * 一个**闭集**动作 JSON，例如
 *
 *     输入  turn on the red light
 *     输出  {"action":"led","color":"red"}
 *
 * 为什么让模型只输出闭集 JSON、把"理解"和"执行"分开：模型只有 0.26M 参数，
 * 让它自由发挥必然出错；而"这句话对应哪个动作"是它学得会的（留出集 43/46），
 * 剩下的"red 到底点亮哪个引脚"是确定性的代码，不该交给模型。
 *
 * 动作表（与 tools/make_cmd_dataset.py 里的定义必须一致）：
 *     {"action":"led","color":"red|green|blue|white|yellow|off"}
 *     {"action":"wifi_scan"}
 *     {"action":"ping","target":"gateway|internet|board"}
 *     {"action":"status"}
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cmd_exec.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "espllm.h"
#include "net_cloud.h"
#include "net_wifi.h"

static const char *TAG = "gw_cmd";

/* 动作 JSON 的 token 预算：prompt（约 10 个 token）也算在里面，所以给宽一点 */
#define CMD_MAX_TOKENS 40

#ifdef CONFIG_GATEWAY_CMD_LED
#include "led_strip.h"
static led_strip_handle_t s_strip;
static bool s_strip_ready;
#endif

void cmd_exec_init(void)
{
#ifdef CONFIG_GATEWAY_CMD_LED
    led_strip_config_t strip = {
        .strip_gpio_num = CONFIG_GATEWAY_LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip, &rmt, &s_strip);
    if (err == ESP_OK) {
        s_strip_ready = true;
        led_strip_clear(s_strip);
        ESP_LOGI(TAG, "onboard RGB led on GPIO%d (ws2812 via RMT)", CONFIG_GATEWAY_LED_GPIO);
    } else {
        ESP_LOGW(TAG, "no addressable led on GPIO%d (%s) -- the led action will say so",
                 CONFIG_GATEWAY_LED_GPIO, esp_err_to_name(err));
    }
#else
    ESP_LOGI(TAG, "led action disabled in menuconfig");
#endif
}

/* --------------------------------------------------------------- 动作实现 -- */

static const char *led_apply(const char *color)
{
    static const struct { const char *name; int r, g, b; } kColors[] = {
        { "red",    24,  0,  0 }, { "green",  0, 24,  0 }, { "blue",   0,  0, 24 },
        { "white",  16, 16, 16 }, { "yellow", 20, 20,  0 }, { "off",    0,  0,  0 },
    };
    for (size_t i = 0; i < sizeof(kColors) / sizeof(kColors[0]); i++) {
        if (strcmp(color, kColors[i].name) != 0) {
            continue;
        }
#ifdef CONFIG_GATEWAY_CMD_LED
        if (!s_strip_ready) {
            return "led driver unavailable";
        }
        esp_err_t err = led_strip_set_pixel(s_strip, 0, kColors[i].r, kColors[i].g, kColors[i].b);
        if (err == ESP_OK) {
            err = led_strip_refresh(s_strip);
        }
        if (err != ESP_OK) {
            return "led write failed";
        }
        return "led set";
#else
        return "led disabled at build time";
#endif
    }
    return "unknown colour";
}

/* ping 的目标是闭集，所以名字到地址的映射写死在代码里，不让模型碰 IP。
 * 端口也一起定死：ICMP 在这条链路上要 raw socket 和额外组件（IDF 5.5 的
 * esp_ping.h 已经变成兼容壳，公开 API 挪到了不导出的目录），所以这里测的是
 * "到目标端口的 TCP 连接时延"——结果里 method 字段如实写着 tcp_connect，
 * 不假装是 ICMP。 */
static const struct { const char *name; const char *ip; int port; } kTargets[] = {
    /* "gateway" 是这个设备自己。走 127.0.0.1，不走 PPP 自己的地址 10.0.0.1：
     * 实测从本机连自己的 PPP 地址是连不上的（lwIP 在那个 netif 上不做回环），
     * 而回环地址本来就是"我自己"的正确含义。 */
    { "gateway",  "127.0.0.1",  80 },      /* 本机（S3 网关自己的 HTTP） */
    { "board",    "10.0.0.2", 28789 },     /* 板子 Agent 的 WebSocket 口 */
    { "internet", "223.5.5.5",  53 },      /* AliDNS，公网可达性最省事的锚点 */
};

static const char *target_ip(const char *target, int *port)
{
    for (size_t i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); i++) {
        if (strcmp(target, kTargets[i].name) == 0) {
            *port = kTargets[i].port;
            return kTargets[i].ip;
        }
    }
    return NULL;
}

/* 连一次，量连接建立的毫秒数。0 = 成功。 */
static int tcp_probe(const char *ip, int port, int *rtt_ms)
{
    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    dst.sin_addr.s_addr = inet_addr(ip);
    if (dst.sin_addr.s_addr == INADDR_NONE) {
        return -1;
    }

    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        return -1;
    }
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);

    int64_t t0 = esp_timer_get_time();
    int rc = connect(s, (struct sockaddr *)&dst, sizeof(dst));
    if (rc != 0 && errno != EINPROGRESS) {
        close(s);
        return -1;
    }
    if (rc != 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
        if (select(s + 1, NULL, &wfds, NULL, &tv) <= 0) {
            close(s);
            return -1;                     /* 超时 */
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            close(s);
            return -1;
        }
    }
    *rtt_ms = (int)((esp_timer_get_time() - t0) / 1000);
    close(s);
    return 0;
}

/* ------------------------------------------------------------- 入口 ------ */

int cmd_exec_run(const char *text, char *out, size_t out_size)
{
    if (text == NULL || text[0] == '\0') {
        return -1;
    }

    /* 板子的 Agent 会把整段会话发过来，取最后一条用户消息即可 */
    char prompt[256];
    snprintf(prompt, sizeof(prompt), "cmd: %.200s\n", text);

    esp_err_t err = espllm_generate(prompt, CMD_MAX_TOKENS);
    if (err != ESP_OK) {
        snprintf(out, out_size, "command model failed: %s", esp_err_to_name(err));
        return -1;
    }

    /* 引擎会把 prompt 回显出来（上游 generate() 的做法），所以从第一个 '{' 开始取。
     * 这一步也是"约束解码"的廉价版本：模型只要吐出一个合法的动作 JSON 就算成功，
     * 吐不出来就在下面拒绝，绝不猜。 */
    const char *raw = espllm_text();
    const char *brace = strchr(raw, '{');
    if (brace == NULL) {
        snprintf(out, out_size, "model said: %.80s", raw);
        ESP_LOGW(TAG, "no action json in %u chars", (unsigned)espllm_text_len());
        return -1;
    }

    char action[24] = { 0 }, color[24] = { 0 }, target[24] = { 0 };
    bool have_action = espllm_json_field(brace, "action", action, sizeof(action));
    if (!have_action) {
        snprintf(out, out_size, "model said: %.80s", brace);
        return -1;
    }

    if (strcmp(action, "led") == 0) {
        (void)espllm_json_field(brace, "color", color, sizeof(color));
        const char *res = led_apply(color);
        snprintf(out, out_size, "{\"action\":\"led\",\"color\":\"%s\",\"result\":\"%s\"}",
                 color, res);
        ESP_LOGI(TAG, "led %s -> %s", color, res);
        return 0;
    }

    if (strcmp(action, "wifi_scan") == 0) {
        int aps = net_wifi_scan_count();
        snprintf(out, out_size, "{\"action\":\"wifi_scan\",\"aps\":%d}", aps);
        ESP_LOGI(TAG, "wifi scan -> %d APs", aps);
        return 0;
    }

    if (strcmp(action, "ping") == 0) {
        (void)espllm_json_field(brace, "target", target, sizeof(target));
        int port = 0;
        const char *ip = target_ip(target, &port);
        int rtt = 0;
        if (ip == NULL) {
            /* 模型给了闭集之外的目标：如实说是目标不认识，不要说成"没回应"——
             * 这两件事的含义完全不同。 */
            snprintf(out, out_size,
                     "{\"action\":\"ping\",\"target\":\"%s\",\"error\":\"unknown target\"}",
                     target);
            ESP_LOGW(TAG, "ping: model asked for an unknown target \"%s\"", target);
            return -1;
        }
        if (tcp_probe(ip, port, &rtt) == 0) {
            snprintf(out, out_size,
                     "{\"action\":\"ping\",\"target\":\"%s\",\"ip\":\"%s\",\"port\":%d,"
                     "\"rtt_ms\":%d,\"method\":\"tcp_connect\"}",
                     target, ip, port, rtt);
            ESP_LOGI(TAG, "ping %s (%s:%d) -> %d ms (tcp connect)", target, ip, port, rtt);
            return 0;
        }
        snprintf(out, out_size,
                 "{\"action\":\"ping\",\"target\":\"%s\",\"error\":\"no reply\","
                 "\"method\":\"tcp_connect\"}", target);
        ESP_LOGW(TAG, "ping %s (%s:%d): no reply", target, ip, port);
        return 0;
    }

    if (strcmp(action, "status") == 0) {
        char body[512];
        int n = snprintf(body, sizeof(body), "{\"action\":\"status\",\"detail\":\"");
        net_cloud_status(body + n, sizeof(body) - (size_t)n - 4);
        size_t len = strlen(body);
        /* 状态里有多行，塞进 JSON 前把控制字符换掉 */
        for (size_t i = 0; i < len; i++) {
            if (body[i] == '\n' || body[i] == '\r' || body[i] == '\t') {
                body[i] = ' ';
            }
        }
        snprintf(body + len, sizeof(body) - len, "\"}");
        snprintf(out, out_size, "%s", body);
        ESP_LOGI(TAG, "status -> %u chars", (unsigned)strlen(body));
        return 0;
    }

    snprintf(out, out_size, "unknown action: %.40s", action);
    ESP_LOGW(TAG, "unknown action %s", action);
    return -1;
}
