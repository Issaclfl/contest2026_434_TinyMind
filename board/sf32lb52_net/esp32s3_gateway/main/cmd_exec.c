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
 *     {"action":"led","color":"blue","effect":"blink","times":3}   闪几下
 *     {"action":"led","color":"red","effect":"breath"}             呼吸
 *     {"action":"wifi_scan"}
 *     {"action":"ping","target":"gateway|internet|board"}
 *     {"action":"status"}
 *     {"action":"temperature"}                                    片内温度传感器
 *     {"action":"time"}                                           SNTP 对时（UTC+8）
 *     {"action":"weather","city":"beijing|…"}                     wttr.in 真数据
 *     {"action":"ask"}                                            这题该问云端
 *
 * 最后三条是"两类不同的活"的证据：temperature 是**读传感器**、weather 是**取网络
 * 数据**、ask 是**把问题转给云端大模型**（本地小模型只判断"该不该问"，不负责复述
 * 和作答——那是 0.26M 参数最不擅长的部分）。
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cmd_exec.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/temperature_sensor.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "espllm.h"
#include "net_cloud.h"
#include "net_wifi.h"

static const char *TAG = "gw_cmd";

/* 动作 JSON 的 token 预算（prompt 也算在里面）。
 *
 * 上游的 generate() **不认 EOS**：它会把预算跑满才开始收尾，所以这个数直接决定
 * 延迟（设备上 ~50 tok/s，110 约 2 秒）。定成 110 是因为最长的动作
 * `{"action":"led","color":"yellow","effect":"blink","times":5}` 是 61 个字符，
 * 而这个 tokenizer 的合并表来自 TinyStories，JSON 几乎一个字符一个 token：
 * 61（答案）+ 约 40（"cmd: " 加口令）= 101，留点余量。 */
#define CMD_MAX_TOKENS 110

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

/* --------------------------------------------------------------- 小工具 -- */

/* 把文本塞进 JSON 字符串前转义（够用：引号、反斜杠、控制字符）。 */
static void json_escape_into(const char *src, char *dst, size_t dst_size)
{
    size_t o = 0;
    for (const char *p = src; *p != '\0' && o + 8 < dst_size; p++) {
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
            continue;
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

/* 取一个整数域。组件里的 espllm_json_field 只认**字符串**值（它的定义就是找
 * "key":"），而动作里的 "times":3 是数字，所以这里自己扫一眼——不然闪灯永远
 * 只闪一下，而且看不出哪里错了。 */
static bool json_int_field(const char *json, const char *key, int *out)
{
    char needle[24];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (p == NULL) {
        return false;
    }
    p += strlen(needle);
    while (*p == ' ') {
        p++;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    *out = atoi(p);
    return true;
}

/* 按字节截断但不能把 UTF-8 字符劈成两半（劈了页面上就是乱码）。 */
static void utf8_truncate(char *s, size_t max_bytes)
{
    size_t n = strlen(s);
    if (n <= max_bytes) {
        return;
    }
    size_t i = max_bytes;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) {
        i--;
    }
    s[i] = '\0';
}

/* --------------------------------------------------------------- 动作实现 -- */

/* 颜色闭集。亮度压得低（24/255）是有意的：板载 WS2812 满亮会晃眼，而且演示
 * 时相机容易过曝。 */
typedef struct { const char *name; int r, g, b; } led_color_t;

static const led_color_t kColors[] = {
    { "red",    24,  0,  0 }, { "green",  0, 24,  0 }, { "blue",   0,  0, 24 },
    { "white",  16, 16, 16 }, { "yellow", 20, 20,  0 }, { "off",    0,  0,  0 },
};

static const led_color_t *color_rgb(const char *name)
{
    for (size_t i = 0; i < sizeof(kColors) / sizeof(kColors[0]); i++) {
        if (strcmp(name, kColors[i].name) == 0) {
            return &kColors[i];
        }
    }
    return NULL;
}

#ifdef CONFIG_GATEWAY_CMD_LED
static esp_err_t led_write(int r, int g, int b)
{
    esp_err_t err = led_strip_set_pixel(s_strip, 0, r, g, b);
    return err == ESP_OK ? led_strip_refresh(s_strip) : err;
}
#endif

/* 颜色 + 两种特效（闪 N 次、呼吸）。
 *
 * 闪与呼吸是**阻塞**做的：调用这条路的任务本来就是 HTTP 任务，闪三下约 1 秒、
 * 呼吸一轮 2 秒，而语音那条腿本来就要 3 秒多——为它引入一个特效任务和队列，
 * 换来的复杂度比省下的 1 秒值钱。*/
static const char *led_apply(const char *color, const char *effect, int times)
{
    const led_color_t *c = color_rgb(color);
    if (c == NULL) {
        return "unknown colour";
    }
#ifdef CONFIG_GATEWAY_CMD_LED
    if (!s_strip_ready) {
        return "led driver unavailable";
    }

    if (effect != NULL && strcmp(effect, "blink") == 0) {
        if (times < 1) {
            times = 1;
        }
        if (times > 10) {
            times = 10;
        }
        for (int i = 0; i < times; i++) {
            if (led_write(c->r, c->g, c->b) != ESP_OK) {
                return "led write failed";
            }
            vTaskDelay(pdMS_TO_TICKS(160));
            if (led_write(0, 0, 0) != ESP_OK) {
                return "led write failed";
            }
            vTaskDelay(pdMS_TO_TICKS(160));
        }
        (void)led_write(c->r, c->g, c->b);   /* 闪完停在亮着 */
        return "led blinked";
    }

    if (effect != NULL && strcmp(effect, "breath") == 0) {
        for (int i = 0; i <= 24; i++) {      /* 一轮约 2 秒：暗 → 亮 → 暗 */
            int k = i <= 12 ? i : 24 - i;
            if (led_write(c->r * k / 12, c->g * k / 12, c->b * k / 12) != ESP_OK) {
                return "led write failed";
            }
            vTaskDelay(pdMS_TO_TICKS(80));
        }
        (void)led_write(c->r, c->g, c->b);
        return "led breathed";
    }

    return led_write(c->r, c->g, c->b) == ESP_OK ? "led set" : "led write failed";
#else
    return "led disabled at build time";
#endif
}

/* ---- 芯片温度：ESP32-S3 片内温度传感器（真读数，不是猜） ------------------ */
static bool chip_temperature(float *out_c)
{
    static temperature_sensor_handle_t s_ts;
    static bool s_tried;
    if (s_ts == NULL) {
        if (s_tried) {
            return false;
        }
        s_tried = true;
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_ts) != ESP_OK) {
            ESP_LOGW(TAG, "no temperature sensor on this chip");
            return false;
        }
        if (temperature_sensor_enable(s_ts) != ESP_OK) {
            s_ts = NULL;
            return false;
        }
    }
    return temperature_sensor_get_celsius(s_ts, out_c) == ESP_OK;
}

/* ---- 网络取文本（天气）：wttr.in 免 key、返回一行纯文本 ------------------- */
static int http_get_text(const char *url, char *out, size_t out_size)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return -1;
    }
    int rc = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        int n = esp_http_client_read(c, out, (int)out_size - 1);
        out[n > 0 ? n : 0] = '\0';
        while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) {
            out[--n] = '\0';
        }
        if (status == 200 && n > 0) {
            rc = 0;
        } else {
            snprintf(out, out_size, "the weather service answered HTTP %d", status);
        }
    } else {
        snprintf(out, out_size, "could not reach the weather service");
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return rc;
}

/* 天气源是闭集城市表（同 ping 的目标表一个道理：不让模型碰 URL）。 */
static const char kCities[][12] = {
    "beijing", "shanghai", "guangzhou", "shenzhen",
    "hangzhou", "chengdu", "wuhan", "xian",
};

static bool city_known(const char *city)
{
    for (size_t i = 0; i < sizeof(kCities) / sizeof(kCities[0]); i++) {
        if (strcmp(city, kCities[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- 对时 -----------------------------------------------------------------
 *
 * 两条腿，因为实测第一条不总通：**这个热点（手机）不回 UDP 123**，SNTP 等 6 秒
 * 也同步不上（而且 IDF 默认还有个最多 5 秒的启动延迟，见 sdkconfig.defaults 里
 * 关掉的 LWIP_SNTP_STARTUP_DELAY）。所以先试 SNTP（只等 1.5 秒），拿不到就用
 * **同一个云端的 HTTP 响应头里的 `Date`**——那条路跟聊天请求走的是同一条 TLS，
 * 实测通，整条动作 1~2 秒。第一次失败后不再等 SNTP（hopeless）。
 *
 * 两条腿拿到的都是 UTC 秒，最后统一按 UTC+8 打印。 */

/* "Sat, 19 Sep 2026 05:12:34 GMT" → UTC 秒。手写解析，不依赖 strptime 的可见性。 */
static int parse_http_date(const char *buf, time_t *out)
{
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int d = 0, y = 0, hh = 0, mi = 0, ss = 0;
    char mon[4] = { 0 };
    if (sscanf(buf, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &hh, &mi, &ss) != 6) {
        return -1;
    }
    int m = 0;
    for (; m < 12; m++) {
        if (strncmp(mon, months[m], 3) == 0) {
            break;
        }
    }
    if (m == 12) {
        return -1;
    }
    struct tm t = { 0 };
    t.tm_mday = d;
    t.tm_mon = m;
    t.tm_year = y - 1900;
    t.tm_hour = hh;
    t.tm_min = mi;
    t.tm_sec = ss;
    setenv("TZ", "UTC", 1);
    tzset();
    *out = mktime(&t);           /* TZ=UTC 下 mktime 就是"把 UTC 墙上时间变回秒" */
    return *out > 0 ? 0 : -1;
}

/* Date 头要在事件回调里接：esp_http_client_get_header() 只认客户端自己
 * esp_http_client_set_header() 设过的**请求**头，读不到响应头（试过——拿到的是
 * NULL）。HTTP_EVENT_ON_HEADER 才是响应头逐条到齐的地方。 */
static esp_err_t on_http_header(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key != NULL &&
        evt->header_value != NULL && strcasecmp(evt->header_key, "Date") == 0) {
        strlcpy((char *)evt->user_data, evt->header_value, 48);
    }
    return ESP_OK;
}

static int http_date_now(time_t *out)
{
    char date[48] = { 0 };
    esp_http_client_config_t cfg = {
        .url = CONFIG_GATEWAY_CLOUD_URL,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http_header,
        .user_data = date,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return -1;
    }
    int rc = -1;
    /* 不要正文，只要响应头；4xx 也没关系，Date 头照样有。 */
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (date[0] != '\0') {
            rc = parse_http_date(date, out);
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return rc;
}

static int sntp_now(char *out, size_t out_size)
{
    static bool started;
    static bool hopeless;          /* 这个网络不回 NTP：别再白等 6 秒 */
    if (!started) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_init();
        started = true;
    }
    bool synced = false;
    if (!hopeless) {
        for (int i = 0; i < 15 && !synced; i++) {     /* 只等 1.5 秒 */
            synced = esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
            if (!synced) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        if (!synced) {
            /* 第一次就等不到，基本可以断定这条路走不通（实测手机热点就是不回
             * UDP 123）。记下来，后面的时间动作直接走 Date 头，省掉那 6 秒。 */
            hopeless = true;
            ESP_LOGW(TAG, "ntp gave no answer; using the HTTP Date header from now on");
        }
    }

    time_t now = 0;
    if (synced) {
        now = time(NULL);
    } else if (http_date_now(&now) == 0) {
        ESP_LOGI(TAG, "ntp did not answer; took the time from the HTTP Date header");
    } else {
        return -1;
    }

    setenv("TZ", "CST-8", 1);                     /* 这块板子只服务一个时区：北京时间 */
    tzset();
    struct tm t;
    localtime_r(&now, &t);
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &t);
    return 0;
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
    char effect[16] = { 0 }, city[20] = { 0 };
    bool have_action = espllm_json_field(brace, "action", action, sizeof(action));
    if (!have_action) {
        snprintf(out, out_size, "model said: %.80s", brace);
        return -1;
    }

    if (strcmp(action, "led") == 0) {
        (void)espllm_json_field(brace, "color", color, sizeof(color));
        (void)espllm_json_field(brace, "effect", effect, sizeof(effect));
        int times = 0;
        (void)json_int_field(brace, "times", &times);
        if (color[0] == '\0' && effect[0] == '\0') {
            snprintf(out, out_size, "{\"action\":\"led\",\"error\":\"no colour given\"}");
            return -1;
        }
        if (color[0] == '\0') {
            strlcpy(color, "white", sizeof(color));   /* "闪一下"没说颜色：给白的 */
        }
        const char *res = led_apply(color, effect, times);
        /* 结果里把特效也带上：演示时页面上一眼能看出"它选了闪/呼吸"，而不是只看到
         * 一个颜色——颜色对而特效丢是这批量产里最容易发生的错（留出集里就有）。 */
        if (effect[0] != '\0') {
            snprintf(out, out_size,
                     "{\"action\":\"led\",\"color\":\"%s\",\"effect\":\"%s\",\"times\":%d,"
                     "\"result\":\"%s\"}", color, effect, times > 0 ? times : 1, res);
        } else {
            snprintf(out, out_size,
                     "{\"action\":\"led\",\"color\":\"%s\",\"result\":\"%s\"}", color, res);
        }
        ESP_LOGI(TAG, "led %s %s%s -> %s", color, effect,
                 times > 0 ? " x" : "", res);
        /* "led set/blinked/breathed" 才算做成；unknown colour、驱动不在线这些
         * 都如实算失败（页面上能看到 result 里写的原因）。 */
        return (strcmp(res, "led set") == 0 || strcmp(res, "led blinked") == 0 ||
                strcmp(res, "led breathed") == 0) ? 0 : -1;
    }

    if (strcmp(action, "temperature") == 0) {
        float celsius = 0;
        if (!chip_temperature(&celsius)) {
            snprintf(out, out_size,
                     "{\"action\":\"temperature\",\"error\":\"no temperature sensor\"}");
            return -1;
        }
        snprintf(out, out_size,
                 "{\"action\":\"temperature\",\"celsius\":%.1f,"
                 "\"source\":\"esp32-s3 internal sensor\"}", (double)celsius);
        ESP_LOGI(TAG, "temperature -> %.1f C", (double)celsius);
        return 0;
    }

    if (strcmp(action, "time") == 0) {
        char stamp[32] = { 0 };
        if (sntp_now(stamp, sizeof(stamp)) != 0) {
            snprintf(out, out_size,
                     "{\"action\":\"time\",\"error\":\"no ntp reply yet (needs the internet)\"}");
            return -1;
        }
        snprintf(out, out_size, "{\"action\":\"time\",\"local\":\"%s\",\"tz\":\"UTC+8\"}", stamp);
        ESP_LOGI(TAG, "time -> %s", stamp);
        return 0;
    }

    if (strcmp(action, "weather") == 0) {
        (void)espllm_json_field(brace, "city", city, sizeof(city));
        if (!city_known(city)) {
            snprintf(out, out_size,
                     "{\"action\":\"weather\",\"city\":\"%s\",\"error\":\"unknown city\"}", city);
            ESP_LOGW(TAG, "weather: model asked for an unknown city \"%s\"", city);
            return -1;
        }
        char url[96];
        snprintf(url, sizeof(url), "https://wttr.in/%s?format=3", city);
        char text[192] = { 0 };
        if (http_get_text(url, text, sizeof(text)) != 0) {
            /* 失败原因是我们自己写的 ASCII，不用清 */
            char esc[256];
            json_escape_into(text, esc, sizeof(esc));
            snprintf(out, out_size, "{\"action\":\"weather\",\"city\":\"%s\",\"error\":\"%s\"}",
                     city, esc);
            return -1;
        }
        char esc[256];
        espllm_utf8_sanitize(text);        /* wttr.in 带天气符号，也可能被读缓冲截断 */
        json_escape_into(text, esc, sizeof(esc));
        snprintf(out, out_size, "{\"action\":\"weather\",\"city\":\"%s\",\"report\":\"%s\"}",
                 city, esc);
        ESP_LOGI(TAG, "weather %s -> %s", city, text);
        return 0;
    }

    if (strcmp(action, "ask") == 0) {
        /* 小模型只判断"这题该问云端"，原话由这里原样转出去——不让它复述。 */
        char answer[600];
        char esc[640];
        int rc = net_cloud_ask_text(text, answer, sizeof(answer));
        espllm_utf8_sanitize(answer);      /* 云端答案可能切在多字节字符中间 */
        json_escape_into(answer, esc, sizeof(esc));
        size_t prefix = strlen("{\"action\":\"ask\",\"answer\":\"");
        size_t room = out_size > prefix + 4 ? out_size - prefix - 4 : 0;
        utf8_truncate(esc, room);                 /* 按调用方的缓冲截断，别切断汉字 */
        snprintf(out, out_size, "{\"action\":\"ask\",\"%s\":\"%s\"}",
                 rc == 0 ? "answer" : "error", esc);
        ESP_LOGI(TAG, "ask -> %s: %.80s", rc == 0 ? "answered" : "failed", answer);
        return rc == 0 ? 0 : -1;
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
