/*
 * Wi-Fi station: the gateway's uplink.
 *
 * Modelled on ESP-IDF's wifi/getting_started/station example, with two
 * deliberate differences.  Power save is off, because a gateway relays other
 * devices' traffic and must not nap between DTIM beacons; and reconnection
 * retries forever by default, because a board that has lost its uplink has no
 * way to say so -- it just reports a connected ppp0 and nothing moves.
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "net_wifi.h"

static const char *TAG = "gw_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_GAVE_UP_BIT   BIT1

static EventGroupHandle_t s_events;
static esp_netif_t *s_sta_netif;
static unsigned s_attempt;

/* ---- NO_AP_FOUND diagnostic --------------------------------------------
 * When the configured SSID is not found, the useful question is "what CAN the
 * radio see?" -- but console output goes into a public repository, so the
 * answer must not name the neighbours.  This reports only counts: how many
 * access points are visible, how many of those are on 2.4 GHz (the only band
 * this radio has), and whether the configured SSID is among them -- with its
 * channel and signal strength when it is. */
static void scan_and_report(void)
{
    wifi_scan_config_t scan = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&scan, true /* block until done */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "diagnostic scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    wifi_ap_record_t *records = calloc(count ? count : 1, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        ESP_LOGW(TAG, "scan: out of memory for %u records", count);
        esp_wifi_clear_ap_list();
        return;
    }
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
        free(records);
        return;
    }

    uint16_t on_24 = 0;
    uint16_t matched_channel = 0;
    int8_t matched_rssi = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (records[i].primary <= 14) {
            on_24++;
        }
        if (matched_channel == 0 &&
            strcmp((const char *)records[i].ssid, CONFIG_GATEWAY_WIFI_SSID) == 0) {
            matched_channel = records[i].primary;
            matched_rssi = records[i].rssi;
        }
    }
    free(records);

    if (matched_channel != 0) {
        ESP_LOGW(TAG, "scan: \"%s\" IS visible -- channel %u (%s), RSSI %d, %u APs total (%u on 2.4 GHz)",
                 CONFIG_GATEWAY_WIFI_SSID, matched_channel,
                 matched_channel <= 14 ? "2.4 GHz" : "5 GHz",
                 matched_rssi, count, on_24);
    } else {
        ESP_LOGW(TAG, "scan: \"%s\" is NOT among %u visible APs (%u of them on 2.4 GHz)",
                 CONFIG_GATEWAY_WIFI_SSID, count, on_24);
        if (count > 0 && on_24 == 0) {
            ESP_LOGW(TAG, "scan: every visible AP is on 5 GHz -- if the target is a phone "
                          "hotspot, switch it to the 2.4 GHz band (iPhone: Maximize "
                          "Compatibility; Android: AP band -> 2.4 GHz)");
        }
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        if (CONFIG_GATEWAY_WIFI_MAX_RETRY == 0 || s_attempt < CONFIG_GATEWAY_WIFI_MAX_RETRY) {
            s_attempt++;
            ESP_LOGW(TAG, "uplink lost (reason %d), attempt %u", ev->reason, s_attempt);
            if (ev->reason == WIFI_REASON_NO_AP_FOUND && (s_attempt == 1 || s_attempt % 5 == 0)) {
                /* Must run before the next connect: a scan is refused while the
                 * station is trying to associate.  Blocking the event loop for
                 * the ~2 s the scan takes is fine here -- nothing else can
                 * happen while the uplink is down anyway. */
                scan_and_report();
            }
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "uplink lost (reason %d), giving up after %u attempts", ev->reason, s_attempt);
            xEventGroupSetBits(s_events, WIFI_GAVE_UP_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        s_attempt = 0;
        ESP_LOGI(TAG, "uplink ready: " IPSTR ", gateway " IPSTR,
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t net_wifi_start(uint32_t timeout_ms)
{
    if (CONFIG_GATEWAY_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "no SSID configured. `idf.py menuconfig` -> SF32LB52 gateway -> Wi-Fi SSID; "
                      "it is kept out of git on purpose.");
        return ESP_ERR_INVALID_STATE;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, CONFIG_GATEWAY_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_GATEWAY_WIFI_PASSWORD, sizeof(cfg.sta.password));
    /* Accept whatever security the access point offers.  The SSID is the
     * operator's choice; refusing a WPA3 or an open network on a technicality
     * would only be a puzzle to debug later. */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    /* With power save on, the station sleeps between beacons and the board
     * sees stalls of hundreds of milliseconds on its own connections. */
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not disable power save: %s", esp_err_to_name(err));
    }

    /* The uplink holds the default route.  The two inherent configs already
     * rank themselves this way (station route_prio 100, PPP 20) and the PPP
     * config does not claim the default, so this states the intent rather than
     * fixing anything. */
    err = esp_netif_set_default_netif(s_sta_netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not set the station as the default interface: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "connecting to \"%s\"", CONFIG_GATEWAY_WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT | WIFI_GAVE_UP_BIT,
                                          pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & WIFI_GAVE_UP_BIT) {
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "no address within %" PRIu32 " ms", timeout_ms);
    return ESP_ERR_TIMEOUT;
}

esp_netif_t *net_wifi_netif(void)
{
    return s_sta_netif;
}
