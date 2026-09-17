/*
 * SF32LB52 Wi-Fi gateway -- entry point.
 *
 * Stands in for the PC-side peer in board/sf32lb52_net/pc_side/.  The board's
 * PPP client does not care who terminates the far end of the UART, so the same
 * `pppd /dev/ttyS0 460800 &` that used to dial a USB-TTL adapter plugged into
 * a PC now dials this board instead, and no board firmware needs to change.
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "net_ppp.h"
#include "net_wifi.h"

static const char *TAG = "gw_main";

#define WIFI_WAIT_MS 30000

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "reinitialising NVS (%s)", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    ESP_LOGI(TAG, "SF32LB52 Wi-Fi gateway");
    ESP_LOGI(TAG, "board runs: pppd /dev/ttyS0 %d &", CONFIG_GATEWAY_UART_BAUD);
    ESP_LOGI(TAG, "wiring: our TX GPIO%d -> PA20, our RX GPIO%d <- PA27, GND to GND, no VCC",
             CONFIG_GATEWAY_UART_TX_GPIO, CONFIG_GATEWAY_UART_RX_GPIO);

    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not create the default event loop: %s", esp_err_to_name(err));
        return;
    }

    /* The uplink has to be up first: the PPP interface is the inside of the
     * NAT, and an inside with no outside would only mislead. */
    err = net_wifi_start(WIFI_WAIT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no Wi-Fi uplink (%s); not starting the PPP server -- "
                      "the board would reach this device but nothing beyond it",
                 esp_err_to_name(err));
        return;
    }

    err = net_ppp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPP server did not start: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "ready -- waiting for the board to dial in");
}
