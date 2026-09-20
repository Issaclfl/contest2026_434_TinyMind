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

#include "net_cloud.h"
#include "net_ppp.h"
#include "net_wifi.h"

#if CONFIG_GATEWAY_LOCAL_LLM
#include "cmd_exec.h"
#include "net_miio.h"
#include "espllm.h"
#include "voice_exec.h"
#endif

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

    /* The uplink normally has to be up first: the PPP interface is the inside
     * of the NAT, and an inside with no outside would only mislead.  With
     * GATEWAY_UPLINK off this is the bench test of the PPP link alone, so the
     * Wi-Fi step is skipped on purpose. */
#if CONFIG_GATEWAY_UPLINK
    err = net_wifi_start(WIFI_WAIT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no Wi-Fi uplink (%s)", esp_err_to_name(err));
#if CONFIG_GATEWAY_LOCAL_LLM
        /* Worth continuing: the local model does not need Wi-Fi, so the board
         * can still get an address and talk to it.  It just will not reach
         * anything beyond this device -- which the log says out loud, so a
         * missing NAPT is not mistaken for a broken model. */
        ESP_LOGW(TAG, "continuing without an uplink: the board will reach this device only "
                      "(the local model is still served)");
#else
        ESP_LOGE(TAG, "not starting the PPP server: the board would reach this device and "
                      "nothing beyond it, and no local model is built in");
        return;
#endif
    }
#else
    ESP_LOGW(TAG, "GATEWAY_UPLINK is off: PPP only, no Wi-Fi and no NAPT.");
    ESP_LOGW(TAG, "the board will reach this device and stop there -- use it to test "
                  "whether the two PPP implementations negotiate, nothing else.");
#endif

    err = net_ppp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPP server did not start: %s", esp_err_to_name(err));
        return;
    }

#if CONFIG_GATEWAY_LOCAL_LLM
    /* The local model is independent of the uplink: it serves on whatever
     * interface has an address -- the PPP link for the board, Wi-Fi for
     * everything else -- and it is the fallback when there is no Wi-Fi at all.
     * A failure here is logged and stepped over: the board still gets routed. */
    err = espllm_init(CONFIG_GATEWAY_LLM_PART_NAME, CONFIG_GATEWAY_LLM_PART_SUBTYPE, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "local model unavailable (%s); still routing, but no local answers",
                 esp_err_to_name(err));
    } else if (espllm_http_start(CONFIG_ESPLM_HTTP_PORT) != ESP_OK) {
        ESP_LOGE(TAG, "local model loaded but its HTTP endpoint did not start");
    } else {
        /* One endpoint, three routes: the cloud (when the uplink is there), the
         * local model as the fallback, and -- when the client asks for the
         * model named "cmd" -- the same local model as a *command* parser whose
         * JSON output is executed on this board.  The board knows neither the
         * cloud nor this decision. */
        cmd_exec_init();
        net_miio_init();
        voice_start();
        /* 同一组路由再开一份 TLS：手机浏览器只在安全上下文里给麦克风，
         * http 页面上"按住说话"是点不动的（见 voice_exec.c 顶部）。 */
        voice_https_start();
        net_cloud_start();
    }
#else
    ESP_LOGI(TAG, "GATEWAY_LOCAL_LLM is off: this device only routes.");
#endif

    ESP_LOGI(TAG, "ready -- waiting for the board to dial in");
}
