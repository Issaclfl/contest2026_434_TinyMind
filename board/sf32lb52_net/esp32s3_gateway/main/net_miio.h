/*
 * 米家设备控制的局域网客户端（miIO / MIoT）。
 *
 * 一句话：把"打开台灯"变成局域网里的一条 UDP 指令，并把结果读回来。
 * 全程不经过小米云 —— 这是"断网也能控制"的前提。
 *
 * 设备表来自被 gitignore 的 sdkconfig（CONFIG_GATEWAY_MIIO_TABLE），格式：
 *     name,ip,token,model;name,ip,token,model;...
 * 例： lamp,192.168.1.23,76f5...（32 位十六进制）,yeelink.light.color3;
 * token 是设备凭据，只允许出现在 sdkconfig / NVS 里，绝不进仓库与日志。
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char name[16];    /* 逻辑名，模型与板子用它，例如 "lamp" */
    char ip[20];
    char token[40];   /* 32 个十六进制字符 */
    char model[28];   /* 例如 "yeelink.light.color3"；仅用于日志与能力判断 */
    int port;         /* 设备端口，默认 54321（表里第 5 个字段可覆盖） */
} miio_device_t;

/* 读设备表并打一条状态日志。设备表为空是合法状态（未配置米家设备）。 */
void net_miio_init(void);

int net_miio_count(void);
const miio_device_t *net_miio_find(const char *name);

/* 列设备表（不含 token），一行一台，用于 / 状态页与 PC 侧自检。 */
int net_miio_list(char *out, size_t out_size);

/* hello 握手：32 字节全 0xFF 包，设备回的后 16 字节是 token。
 * 2018 年后的固件多数不再回明文 token，所以这里只当诊断用；
 * 真正发指令一律用设备表里的 token。 */
int net_miio_hello(const char *name, char *out, size_t out_size);

/* 执行一个动作：
 *   op = "on" | "off" | "toggle" | "brightness"(value 1..100) |
 *        "color_temp"(value 开尔文 1700..6500) | "info" | "prop"(value 忽略，
 *        用 value2 指定属性名) | "raw"(method=value2 的字符串，params 见实现)
 * 成功返回 0，并把一行 JSON 结果写进 out（既给板子播报，也给 PC 侧核对）。 */
int net_miio_run(const char *name, const char *op, int value, const char *arg,
    char *out, size_t out_size);

/* 给 model:"miio" 这条测试路由用的确定性文本形式：
 *   "lamp on" / "lamp off" / "lamp brightness 60" / "lamp color_temp 4000"
 *   "lamp info" / "lamp prop power" / "lamp raw get_prop [\"power\"]"
 * 不含任何模型推理 —— 先用它把协议链路单独验通。 */
int net_miio_run_text(const char *text, char *out, size_t out_size);

/* 把一次执行结果翻成给用户听的一句话（中文纯文本）。返回 0 表示"成功且已翻译"，-1 表示失败。
 * 机器可读的 JSON 仍由 net_miio_run 给出。 */
int net_miio_zh(const char *cmd_text, const char *result_json, char *out, size_t out_size);
