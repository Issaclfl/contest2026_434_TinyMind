/*
 * 米家设备控制的局域网客户端（miIO / MIoT）—— 实现。
 *
 * 报文格式与 python-miio 的 miio/protocol.py 逐字段对齐（那份是事实标准，
 * 官方的设备模拟器也用它）：
 *
 *   0..1    magic        0x21 0x31                    （"!1"）
 *   2..3    length       大端 uint16 = 密文长度 + 32
 *   4..7    unknown      0
 *   8..11   device_id
 *   12..15  ts           大端 uint32（本实现填 0）
 *   16..31  checksum     MD5(header16 + token + 密文)
 *   32..    密文         AES-128-CBC(PKCS7(json + '\0'), key=MD5(token),
 *                                     iv=MD5(key + token))
 *
 * 收到的包**不校验 checksum**：解密后能解出 JSON 就说明 token 是对的，
 * 免得在一处容易搞错的细节上跟不同固件扯皮（python-miio 自己也把 hello
 * 的 checksum 当裸字节收）。
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include "net_miio.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/aes.h"
#include "mbedtls/md5.h"

#include "sdkconfig.h"

static const char *TAG = "gw_miio";

#define MIIO_PORT 54321
#define MIIO_MAX_DEVICES 4
#define MIIO_PAYLOAD_MAX 512        /* 指令都很小；回包另给 1024 */
#define MIIO_RESP_MAX 1024
#define MIIO_TIMEOUT_MS 2000
#define MIIO_TRIES 3

static miio_device_t s_dev[MIIO_MAX_DEVICES];
static int s_dev_count;

/* --------------------------------------------------------------- 小工具 -- */

static void hex_to_bin(const char *hex, uint8_t *out, size_t out_len)
{
    for (size_t i = 0; i < out_len; i++) {
        unsigned v = 0;
        sscanf(hex + i * 2, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

/* MD5(token) 与 MD5(key + token)：两条腿都用同一对 key/iv（与参考实现一致）。 */
static void miio_key_iv(const uint8_t token[16], uint8_t key[16], uint8_t iv[16])
{
    mbedtls_md5(token, 16, key);
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, key, 16);
    mbedtls_md5_update(&ctx, token, 16);
    mbedtls_md5_finish(&ctx, iv);
    mbedtls_md5_free(&ctx);
}

static size_t pkcs7_pad(uint8_t *buf, size_t len)
{
    size_t pad = 16 - (len % 16);
    for (size_t i = 0; i < pad; i++) {
        buf[len + i] = (uint8_t)pad;
    }
    return len + pad;
}

static size_t pkcs7_unpad(uint8_t *buf, size_t len)
{
    if (len == 0 || len % 16 != 0) {
        return 0;
    }
    size_t pad = buf[len - 1];
    if (pad == 0 || pad > 16 || pad > len) {
        return len;              /* 不是合法填充：原样返回，让 JSON 解析去判 */
    }
    return len - pad;
}

/* 组一个要发出去的包：header + checksum + 密文。返回总长度。 */
static size_t miio_build(const uint8_t token[16], const char *json, uint8_t *pkt,
    size_t pkt_cap)
{
    uint8_t key[16], iv[16];
    miio_key_iv(token, key, iv);

    uint8_t plain[MIIO_PAYLOAD_MAX];
    size_t plen = strlen(json);
    if (plen + 2 > sizeof(plain)) {
        return 0;
    }
    memcpy(plain, json, plen);
    plain[plen++] = '\0';                      /* 参考实现：json 末尾补一个 0 */
    plen = pkcs7_pad(plain, plen);

    if (32 + plen > pkt_cap) {
        return 0;
    }

    uint8_t *hdr = pkt;                        /* 16 字节头 */
    memset(hdr, 0, 16);
    hdr[0] = 0x21;
    hdr[1] = 0x31;
    uint16_t total = (uint16_t)(32 + plen);
    hdr[2] = (uint8_t)(total >> 8);
    hdr[3] = (uint8_t)(total & 0xff);
    /* 4..15 保持 0：unknown / device_id / ts 都用 0 */

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    uint8_t civ[16];
    memcpy(civ, iv, 16);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, plen, civ, plain, pkt + 32);
    mbedtls_aes_free(&aes);

    /* checksum = MD5(header16 + token + 密文) */
    uint8_t digest[16];
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, hdr, 16);
    mbedtls_md5_update(&ctx, token, 16);
    mbedtls_md5_update(&ctx, pkt + 32, plen);
    mbedtls_md5_finish(&ctx, digest);
    mbedtls_md5_free(&ctx);
    memcpy(pkt + 16, digest, 16);

    return total;
}

/* 把收到的密文解成明文 JSON（就地），返回明文长度；失败返回 0。 */
static size_t miio_parse(const uint8_t token[16], const uint8_t *pkt, size_t len,
    uint8_t *out, size_t out_cap)
{
    if (len < 32) {
        return 0;
    }
    size_t clen = len - 32;
    if (clen == 0 || clen % 16 != 0 || clen > out_cap) {
        return 0;
    }

    uint8_t key[16], iv[16];
    miio_key_iv(token, key, iv);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, key, 128);
    uint8_t civ[16];
    memcpy(civ, iv, 16);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, clen, civ, pkt + 32, out);
    mbedtls_aes_free(&aes);

    size_t plen = pkcs7_unpad(out, clen);
    if (plen == 0 || plen >= out_cap) {
        return 0;
    }
    out[plen] = '\0';
    /* 末尾可能还带参考实现补的那个 0 */
    while (plen > 0 && out[plen - 1] == '\0') {
        out[--plen] = '\0';
    }
    return plen;
}

/* ------------------------------------------------------------ 收发一次 -- */

/* 发一条指令并收回包。resp_json 里放解密后的应答（已去掉尾部空白）。 */
static int miio_call(const miio_device_t *dev, const char *method, const char *params,
    int id, char *resp_json, size_t resp_size)
{
    uint8_t token[16];
    if (strlen(dev->token) < 32) {
        snprintf(resp_json, resp_size, "{\"error\":\"token 长度不对\"}");
        return -1;
    }
    hex_to_bin(dev->token, token, 16);

    char json[MIIO_PAYLOAD_MAX];
    if (params != NULL && params[0] != '\0') {
        snprintf(json, sizeof(json), "{\"id\":%d,\"method\":\"%s\",\"params\":%s}", id,
            method, params);
    } else {
        snprintf(json, sizeof(json), "{\"id\":%d,\"method\":\"%s\"}", id, method);
    }

    uint8_t pkt[MIIO_PAYLOAD_MAX + 64];
    size_t total = miio_build(token, json, pkt, sizeof(pkt));
    if (total == 0) {
        snprintf(resp_json, resp_size, "{\"error\":\"组包失败（指令太长？）\"}");
        return -1;
    }

    /* UDP：miIO 会丢包，重试几次是常态ではなく必须 */
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        snprintf(resp_json, resp_size, "{\"error\":\"socket: %d\"}", errno);
        return -1;
    }
    struct timeval tv = { .tv_sec = MIIO_TIMEOUT_MS / 1000,
                          .tv_usec = (MIIO_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)dev->port);
    inet_pton(AF_INET, dev->ip, &dst.sin_addr);

    int ret = -1;
    for (int attempt = 1; attempt <= MIIO_TRIES && ret != 0; attempt++) {
        if (sendto(fd, pkt, total, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
            ESP_LOGW(TAG, "%s: sendto 失败 errno=%d", dev->name, errno);
            continue;
        }

        uint8_t rx[MIIO_RESP_MAX + 64];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(fd, rx, sizeof(rx), 0, (struct sockaddr *)&from, &flen);
        if (n <= 32) {
            ESP_LOGW(TAG, "%s: 第 %d 次没收到回包（n=%d）", dev->name, attempt, n);
            continue;
        }

        uint8_t plain[MIIO_RESP_MAX];
        size_t plen = miio_parse(token, rx, (size_t)n, plain, sizeof(plain));
        if (plen == 0) {
            ESP_LOGW(TAG, "%s: 第 %d 次解不开回包（token 不对？）", dev->name, attempt);
            continue;
        }
        snprintf(resp_json, resp_size, "%s", (char *)plain);
        ret = 0;
    }

    close(fd);
    if (ret != 0 && resp_json[0] == '\0') {
        snprintf(resp_json, resp_size, "{\"error\":\"设备没回应（%d 次）\"}", MIIO_TRIES);
    }
    return ret;
}

/* -------------------------------------------------------------- 动作表 -- */

/* 把应答里的 result / error 摘出来，压成一行给板子念、给 PC 看。 */
static void miio_summarize(const char *resp, const char *dev_name, char *out,
    size_t out_size)
{
    cJSON *doc = cJSON_Parse(resp);
    if (doc == NULL) {
        snprintf(out, out_size, "{\"device\":\"%s\",\"ok\":false,\"raw\":\"%s\"}",
            dev_name, resp);
        return;
    }

    cJSON *err = cJSON_GetObjectItem(doc, "error");
    cJSON *result = cJSON_GetObjectItem(doc, "result");
    char res[512] = "";
    if (result != NULL) {
        char *s = cJSON_PrintUnformatted(result);
        if (s != NULL) {
            snprintf(res, sizeof(res), "%s", s);
            free(s);
        }
    }
    if (err != NULL) {
        char *s = cJSON_PrintUnformatted(err);
        snprintf(out, out_size, "{\"device\":\"%s\",\"ok\":false,\"error\":%s}", dev_name,
            s != NULL ? s : "\"?\"");
        free(s);
    } else {
        snprintf(out, out_size, "{\"device\":\"%s\",\"ok\":true,\"result\":%s}", dev_name,
            res[0] != '\0' ? res : "null");
    }
    cJSON_Delete(doc);
}

int net_miio_run(const char *name, const char *op, int value, const char *arg, char *out,
    size_t out_size)
{
    const miio_device_t *dev = net_miio_find(name != NULL ? name : "lamp");
    if (dev == NULL) {
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"没有这台设备：%s\"}",
            name != NULL ? name : "lamp");
        return -1;
    }
    if (op == NULL) {
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"没有动作\"}");
        return -1;
    }

    char params[256] = "";
    const char *method = NULL;
    int id = (int)(esp_timer_get_time() & 0x7fff);

    if (strcmp(op, "on") == 0) {
        method = "set_power";
        strcpy(params, "[\"on\",\"smooth\",500]");
    } else if (strcmp(op, "off") == 0) {
        method = "set_power";
        strcpy(params, "[\"off\",\"smooth\",500]");
    } else if (strcmp(op, "toggle") == 0) {
        method = "toggle";
        strcpy(params, "[]");
    } else if (strcmp(op, "brightness") == 0) {
        if (value < 1) {
            value = 1;
        }
        if (value > 100) {
            value = 100;
        }
        method = "set_bright";
        snprintf(params, sizeof(params), "[%d,\"smooth\",500]", value);
    } else if (strcmp(op, "color_temp") == 0) {
        if (value < 1700) {
            value = 1700;
        }
        if (value > 6500) {
            value = 6500;
        }
        method = "set_ct_abx";
        snprintf(params, sizeof(params), "[%d,\"smooth\",500]", value);
    } else if (strcmp(op, "info") == 0) {
        method = "miIO.info";
    } else if (strcmp(op, "prop") == 0) {
        method = "get_prop";
        snprintf(params, sizeof(params), "[\"%s\"]", (arg != NULL && arg[0]) ? arg : "power");
    } else if (strcmp(op, "raw") == 0) {
        /* PC 侧探真机用的后门：动作就是 method，参数就是原样 JSON。
         * 只走局域网，不经云端；写死在固件里，不接受外部任意指令。 */
        if (arg == NULL || arg[0] == '\0') {
            snprintf(out, out_size, "{\"ok\":false,\"error\":\"raw 需要 method\"}");
            return -1;
        }
        method = arg;
        params[0] = '\0';
    } else {
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"不认识的动作：%s\"}", op);
        return -1;
    }

    char resp[MIIO_RESP_MAX] = "";
    int rc = miio_call(dev, method, params, id, resp, sizeof(resp));
    if (rc != 0) {
        snprintf(out, out_size, "{\"device\":\"%s\",\"ok\":false,\"error\":\"%s\"}",
            dev->name, resp);
        return rc;
    }

    miio_summarize(resp, dev->name, out, out_size);
    ESP_LOGI(TAG, "%s %s -> %s", dev->name, method, out);
    return 0;
}

/* -------------------------------------------------- 确定性文本 → 动作 -- */

int net_miio_run_text(const char *text, char *out, size_t out_size)
{
    if (text == NULL || text[0] == '\0') {
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"空指令\"}");
        return -1;
    }

    char buf[192];
    snprintf(buf, sizeof(buf), "%s", text);

    /* 第一段是设备名，剩下的是动作 */
    char *save = NULL;
    char *dev = strtok_r(buf, " \t", &save);
    char *op = strtok_r(NULL, " \t", &save);
    char *arg = strtok_r(NULL, " \t", &save);
    if (dev == NULL || op == NULL) {
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"用法：<设备> <动作> [值]\"}");
        return -1;
    }

    int value = (arg != NULL) ? atoi(arg) : 0;
    return net_miio_run(dev, op, value, arg, out, out_size);
}

/* ------------------------------------------------------------ 设备表 -- */

int net_miio_count(void)
{
    return s_dev_count;
}

const miio_device_t *net_miio_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_dev_count; i++) {
        if (strcmp(s_dev[i].name, name) == 0) {
            return &s_dev[i];
        }
    }
    return NULL;
}

int net_miio_list(char *out, size_t out_size)
{
    size_t o = 0;
    o += snprintf(out + o, out_size - o, "miio devices: %d", s_dev_count);
    for (int i = 0; i < s_dev_count && o < out_size; i++) {
        o += snprintf(out + o, out_size - o, "\n  %s @ %s (%s)", s_dev[i].name, s_dev[i].ip,
            s_dev[i].model[0] ? s_dev[i].model : "?");
    }
    return (int)o;
}

void net_miio_init(void)
{
    s_dev_count = 0;
    const char *table = CONFIG_GATEWAY_MIIO_TABLE;
    if (table == NULL || table[0] == '\0') {
        ESP_LOGI(TAG, "未配置米家设备（CONFIG_GATEWAY_MIIO_TABLE 为空）");
        return;
    }

    char buf[512];
    snprintf(buf, sizeof(buf), "%s", table);

    char *save = NULL;
    for (char *row = strtok_r(buf, ";", &save); row != NULL && s_dev_count < MIIO_MAX_DEVICES;
         row = strtok_r(NULL, ";", &save)) {
        miio_device_t *d = &s_dev[s_dev_count];
        memset(d, 0, sizeof(*d));
        d->port = MIIO_PORT;
        char *c1 = strchr(row, ',');
        if (c1 == NULL) {
            continue;
        }
        *c1 = '\0';
        char *c2 = strchr(c1 + 1, ',');
        if (c2 == NULL) {
            continue;
        }
        *c2 = '\0';
        char *c3 = strchr(c2 + 1, ',');
        if (c3 != NULL) {
            *c3 = '\0';
            snprintf(d->model, sizeof(d->model), "%s", c3 + 1);
        }
        snprintf(d->name, sizeof(d->name), "%s", row);
        snprintf(d->ip, sizeof(d->ip), "%s", c1 + 1);
        snprintf(d->token, sizeof(d->token), "%s", c2 + 1);
        if (c3 != NULL) {
            char *c4 = strchr(c3 + 1, ',');
            if (c4 != NULL) {
                int p = atoi(c4 + 1);
                if (p > 0 && p < 65536) {
                    d->port = p;
                }
            }
        }
        if (d->name[0] == '\0' || d->ip[0] == '\0' || strlen(d->token) < 32) {
            continue;              /* 半截的行直接丢，别让坏配置变成诡异行为 */
        }
        s_dev_count++;
    }

    ESP_LOGI(TAG, "设备表：%d 台", s_dev_count);
    for (int i = 0; i < s_dev_count; i++) {
        ESP_LOGI(TAG, "  %s @ %s (%s)", s_dev[i].name, s_dev[i].ip,
            s_dev[i].model[0] ? s_dev[i].model : "?");
    }
}

int net_miio_hello(const char *name, char *out, size_t out_size)
{
    const miio_device_t *dev = net_miio_find(name != NULL ? name : "lamp");
    if (dev == NULL) {
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"没有这台设备\"}");
        return -1;
    }

    uint8_t hello[32];
    memset(hello, 0xff, sizeof(hello));
    hello[0] = 0x21;
    hello[1] = 0x31;
    hello[2] = 0x00;
    hello[3] = 0x20;                      /* length = 32，全 0xFF 的裸包 */

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"socket\"}");
        return -1;
    }
    struct timeval tv = { .tv_sec = MIIO_TIMEOUT_MS / 1000,
                          .tv_usec = (MIIO_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)dev->port);
    inet_pton(AF_INET, dev->ip, &dst.sin_addr);

    int ret = -1;
    if (sendto(fd, hello, sizeof(hello), 0, (struct sockaddr *)&dst, sizeof(dst)) >= 0) {
        uint8_t rx[64];
        int n = recvfrom(fd, rx, sizeof(rx), 0, NULL, NULL);
        if (n == 32) {
            char hex[33];
            for (int i = 0; i < 16; i++) {
                snprintf(hex + i * 2, 3, "%02x", rx[16 + i]);
            }
            /* 只报告"收到/没收到"和 token 是否与配置一致，绝不打印 token 本身 */
            bool same = (strncasecmp(hex, dev->token, 32) == 0);
            snprintf(out, out_size,
                "{\"ok\":true,\"handshake\":\"replied\",\"token_matches_config\":%s}",
                same ? "true" : "false");
            ret = 0;
        } else {
            snprintf(out, out_size,
                "{\"ok\":false,\"handshake\":\"no reply (n=%d)\",\"note\":\"2018+ 固件通常不回明文 token，用配置里的 token 直接发指令即可\"}",
                n);
        }
    } else {
        snprintf(out, out_size, "{\"ok\":false,\"error\":\"sendto: %d\"}", errno);
    }
    close(fd);
    return ret;
}

/* --------------------------------------------------- 给板子念的一句话 -- */

/* 板子的播报层认纯文本（中文），所以这里把执行结果翻成一句话；
 * 机器可读的那份 JSON 由调用方另外带走（net_cloud 里拼进 miio_result 字段）。
 * 只做翻译，不做判断——成功与否看 net_miio_run 的结果。 */
int net_miio_zh(const char *cmd_text, const char *result_json, char *out, size_t out_size)
{
    char label[24] = "设备";
    char op[24] = "";
    int value = 0;

    if (cmd_text != NULL) {
        char buf[192];
        snprintf(buf, sizeof(buf), "%s", cmd_text);
        char *save = NULL;
        char *dev = strtok_r(buf, " ", &save);
        char *o = strtok_r(NULL, " ", &save);
        char *arg = strtok_r(NULL, " ", &save);
        if (o != NULL) {
            snprintf(op, sizeof(op), "%s", o);
        }
        if (arg != NULL) {
            value = atoi(arg);
        }
        /* 逻辑名 -> 说给用户听的名字。加设备时在这里补一行即可。 */
        if (dev != NULL && strcmp(dev, "lamp") == 0) {
            snprintf(label, sizeof(label), "台灯");
        } else if (dev != NULL && strcmp(dev, "ac") == 0) {
            snprintf(label, sizeof(label), "空调");
        } else if (dev != NULL && dev[0] != '\0') {
            snprintf(label, sizeof(label), "%s", dev);
        }
    }

    bool ok = (result_json != NULL) && (strstr(result_json, "ok\":true") != NULL);
    if (!ok && result_json != NULL && strstr(result_json, "没有这台设备") != NULL) {
        snprintf(out, out_size, "没有找到这台设备");
        return -1;
    }
    if (!ok) {
        snprintf(out, out_size, "%s没有回应", label);
        return -1;
    }

    if (strcmp(op, "on") == 0) {
        snprintf(out, out_size, "%s已打开", label);
    } else if (strcmp(op, "off") == 0) {
        snprintf(out, out_size, "%s已关闭", label);
    } else if (strcmp(op, "toggle") == 0) {
        snprintf(out, out_size, "%s已切换", label);
    } else if (strcmp(op, "brightness") == 0) {
        snprintf(out, out_size, "%s亮度已调到 %d", label, value);
    } else if (strcmp(op, "color_temp") == 0) {
        snprintf(out, out_size, "%s色温已调到 %dK", label, value);
    } else if (strcmp(op, "info") == 0) {
        snprintf(out, out_size, "%s状态正常", label);
    } else if (strcmp(op, "prop") == 0 && result_json != NULL) {
        if (strstr(result_json, "\"on\"") != NULL) {
            snprintf(out, out_size, "%s现在是开着的", label);
        } else if (strstr(result_json, "\"off\"") != NULL) {
            snprintf(out, out_size, "%s现在是关着的", label);
        } else {
            snprintf(out, out_size, "%s状态已读回", label);
        }
    } else {
        snprintf(out, out_size, "%s已执行", label);
    }
    return 0;
}
