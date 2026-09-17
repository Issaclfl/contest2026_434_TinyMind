/*
 * PPP server + NAPT: the board dials in on a UART, we NAT it out via Wi-Fi.
 *
 * The board (SF32LB52, NuttX) already speaks standard PPP as a client, so our
 * side has to be the server.  ESP-IDF ships that as lwIP's PPP_SERVER mode
 * behind CONFIG_LWIP_PPP_SERVER_SUPPORT -- but it ships no server example (the
 * old pppos_client example is gone from the tree and the only PPP code left
 * under examples/ is a *client* helper), so this file is written directly
 * against esp_netif_ppp.h, with the transport half modelled on that client's
 * UART pump.
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

#include "net_ppp.h"

static const char *TAG = "gw_ppp";

/*
 * Every one of these misconfigurations fails silently at runtime, which on a
 * board with one console and no debugger costs an afternoon.  A failed build
 * costs a minute, so they are compile-time errors instead.
 */
#if !CONFIG_LWIP_PPP_SUPPORT
#error "CONFIG_LWIP_PPP_SUPPORT is off: lwIP was built without PPP."
#endif
#if !CONFIG_PPP_SUPPORT
/* In C code CONFIG_PPP_SUPPORT only exists as ESP-IDF's compatibility alias
 * for CONFIG_LWIP_PPP_SUPPORT, and it being undefined means lwIP was built
 * without PPP.  Note that this cannot check the *other* gate: whether
 * esp_netif's CMakeLists actually compiled the glue.  If it did not, the
 * failure appears at link time as undefined references to
 * esp_netif_ppp_set_params() and friends -- tools/fix_idf_ppp_gate.py is what
 * fixes that. */
#error "CONFIG_PPP_SUPPORT is undefined: lwIP was built without PPP."
#endif
#if !CONFIG_LWIP_PPP_SERVER_SUPPORT
/* Without it, esp_netif_ppp_set_params() still returns ESP_OK but discards
 * every server-side field, so the peer is handed a nonsense address and the
 * symptom looks like an IPCP bug. */
#error "CONFIG_LWIP_PPP_SERVER_SUPPORT is off: the server-side addresses would be ignored."
#endif
#if CONFIG_LWIP_PPP_VJ_HEADER_COMPRESSION
#error "Disable CONFIG_LWIP_PPP_VJ_HEADER_COMPRESSION: NAPT cannot read VJ-compressed headers (lwip/Kconfig says so)."
#endif
#if !CONFIG_LWIP_IP_FORWARD || !CONFIG_LWIP_IPV4_NAPT
#error "CONFIG_LWIP_IP_FORWARD and CONFIG_LWIP_IPV4_NAPT must both be on, or the board has no route off this device."
#endif

#define PPP_UP_BIT         BIT0

#define PPP_RX_TASK_STACK  4096
#define PPP_RX_TASK_PRIO   10
/* 2048 rather than the 1024 the client example uses: a full 1500-byte MTU
 * frame can arrive faster than the task drains it, and an overrun means a lost
 * AHDLC frame -- PPP cannot ask for a retransmit, it just stalls. */
#define PPP_RX_BUF_SIZE    2048
#define PPP_TX_BUF_SIZE    2048
#define PPP_UART_QUEUE_LEN 16

#define PPP_SUP_TASK_STACK 3072
#define PPP_SUP_TASK_PRIO  4
#define PPP_SUP_PERIOD_MS  1000

static esp_netif_t *s_ppp_netif;
static EventGroupHandle_t s_ppp_events;
static QueueHandle_t s_uart_queue;
static unsigned s_sessions;

/* --- transport ------------------------------------------------------------ */

static esp_err_t ppp_transmit(void *handle, void *buffer, size_t len)
{
    (void)handle;
    /* Copies into the driver's TX ring and returns.  The ring matters: without
     * one this would hold the lwIP thread for the ~32 ms a full-MTU frame
     * takes to clock out at 460800 baud. */
    uart_write_bytes(CONFIG_GATEWAY_UART_PORT, buffer, len);
    return ESP_OK;
}

static esp_netif_driver_ifconfig_t s_driver_cfg = {
    /* Must not be NULL: esp_netif reads a NULL handle as "no driver attached". */
    .handle = (void *)1,
    .transmit = ppp_transmit,
};

static esp_err_t ppp_uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = CONFIG_GATEWAY_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(CONFIG_GATEWAY_UART_PORT, PPP_RX_BUF_SIZE,
                                       PPP_TX_BUF_SIZE, PPP_UART_QUEUE_LEN, &s_uart_queue, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK(uart_param_config(CONFIG_GATEWAY_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(CONFIG_GATEWAY_UART_PORT,
                                CONFIG_GATEWAY_UART_TX_GPIO, CONFIG_GATEWAY_UART_RX_GPIO,
                                UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* Without an RX timeout the driver only posts an event once its FIFO
     * threshold is crossed, which the short frames LCP and IPCP trade never
     * do.  Negotiation then times out with both sides looking idle. */
    ESP_ERROR_CHECK(uart_set_rx_timeout(CONFIG_GATEWAY_UART_PORT, 1));
    return ESP_OK;
}

/* --- events -------------------------------------------------------------- */

static void on_ppp_status(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    /* `data` is deliberately not dereferenced: the phase and error sites post
     * the address of a local esp_netif pointer (&netif) while the
     * connect-failure site posts the pointer itself.  There is only one PPP
     * interface here, so there is nothing to compare against anyway. */
    (void)arg;
    (void)base;
    (void)data;

    switch (id) {
    case NETIF_PPP_PHASE_ESTABLISH:
        ESP_LOGI(TAG, "peer reachable, LCP established");
        break;
    case NETIF_PPP_PHASE_NETWORK:
        ESP_LOGI(TAG, "negotiating IP addresses");
        break;
    case NETIF_PPP_PHASE_RUNNING:
        ESP_LOGI(TAG, "session running");
        break;
    case NETIF_PPP_PHASE_TERMINATE:
        ESP_LOGW(TAG, "peer asked to close the session");
        break;
    case NETIF_PPP_PHASE_DISCONNECT:
        ESP_LOGW(TAG, "link down");
        break;
    case NETIF_PPP_PHASE_DEAD:
        ESP_LOGW(TAG, "link dead");
        break;
    case NETIF_PPP_CONNECT_FAILED:
        ESP_LOGE(TAG, "could not start the PPP session");
        break;
    case NETIF_PPP_ERRORAUTHFAIL:
        ESP_LOGE(TAG, "peer demanded authentication, which we do not offer");
        break;
    case NETIF_PPP_ERRORPEERDEAD:
        ESP_LOGW(TAG, "peer stopped responding");
        break;
    case NETIF_PPP_ERRORPROTOCOL:
        ESP_LOGE(TAG, "protocol error during negotiation");
        break;
    case NETIF_PPP_ERRORDEVICE:
        ESP_LOGE(TAG, "the UART transport was rejected");
        break;
    case NETIF_PPP_ERROROPEN:
        ESP_LOGE(TAG, "could not open a PPP session");
        break;
    case NETIF_PPP_ERRORNONE:
        break;
    default:
        ESP_LOGD(TAG, "PPP status %" PRId32, id);
        break;
    }
}

static void on_ppp_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;

    const ip_event_got_ip_t *ev = data;
    if (ev->esp_netif != s_ppp_netif) {
        return;
    }

    s_sessions++;
    ESP_LOGI(TAG, "session %u up: we are " IPSTR ", board is %s",
             s_sessions, IP2STR(&ev->ip_info.ip), CONFIG_GATEWAY_PPP_PEER_IP);

#if CONFIG_GATEWAY_UPLINK
    /* NAPT belongs on the PPP interface, not the Wi-Fi one: esp_netif's own
     * guide is to enable it on the interface facing the target network, and
     * the implementation switches NAPT off on every other interface.  It fails
     * on an interface that is not up yet, so this is the earliest correct
     * moment for it. */
    esp_err_t err = esp_netif_napt_enable(s_ppp_netif);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NAPT on: the board's traffic now leaves via the Wi-Fi station");
    } else {
        ESP_LOGE(TAG, "NAPT failed (%s): the board can reach us but not the internet",
                 esp_err_to_name(err));
    }
#else
    ESP_LOGW(TAG, "GATEWAY_UPLINK is off, so NAPT is deliberately not enabled: "
                  "the board can reach this device but not the internet");
#endif

    xEventGroupSetBits(s_ppp_events, PPP_UP_BIT);
}

static void on_ppp_lost_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;

    xEventGroupClearBits(s_ppp_events, PPP_UP_BIT);
    ESP_LOGW(TAG, "session down");
    /* Observed on hardware: the board does not drop its own ppp0 when the peer
     * disappears -- net_status keeps reporting the old address while nothing
     * moves.  So say what to do about it instead of leaving it as a mystery. */
    ESP_LOGW(TAG, "if the board still reports a connected ppp0, rerun `pppd /dev/ttyS0 %d &` on it",
             CONFIG_GATEWAY_UART_BAUD);
}

/* --- tasks --------------------------------------------------------------- */

static void ppp_rx_task(void *arg)
{
    (void)arg;

    uint8_t *buf = malloc(PPP_RX_BUF_SIZE);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no memory for the RX buffer");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        uart_event_t ev;
        if (xQueueReceive(s_uart_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (ev.type == UART_DATA) {
            /* Drain until the ring is empty; one read per event would leave a
             * full-MTU frame split across several events. */
            for (;;) {
                size_t buffered = 0;
                if (uart_get_buffered_data_len(CONFIG_GATEWAY_UART_PORT, &buffered) != ESP_OK || buffered == 0) {
                    break;
                }
                int n = uart_read_bytes(CONFIG_GATEWAY_UART_PORT, buf, PPP_RX_BUF_SIZE, 0);
                if (n <= 0) {
                    break;
                }
                esp_netif_receive(s_ppp_netif, buf, (size_t)n, NULL);
            }
        } else if (ev.type == UART_BUFFER_FULL || ev.type == UART_FIFO_OVF) {
            ESP_LOGE(TAG, "UART overrun (%d): a frame was lost, so PPP will stall until it times out",
                     ev.type);
            uart_flush_input(CONFIG_GATEWAY_UART_PORT);
            xQueueReset(s_uart_queue);
        }
    }
}

/*
 * Re-arms the listener and explains the silence.
 *
 * Two jobs, both about not being able to see the board: lwIP needs ppp_listen()
 * called again after a session dies before it will accept the next peer, and a
 * session that never starts is indistinguishable from a miswire unless
 * something says so.
 */
static void ppp_supervisor_task(void *arg)
{
    (void)arg;

    const TickType_t period = pdMS_TO_TICKS(PPP_SUP_PERIOD_MS);
    TickType_t since_arm = 0;
    bool hinted = false;

    for (;;) {
        vTaskDelay(period);
        since_arm += period;

        if (net_ppp_is_up()) {
            hinted = false;
            continue;
        }

        const unsigned idle_s = (unsigned)((since_arm * PPP_SUP_PERIOD_MS) / 1000);

        if (!hinted && idle_s >= CONFIG_GATEWAY_NO_SESSION_HINT_SEC) {
            hinted = true;
            ESP_LOGW(TAG, "no session after %u s -- check, in this order:", idle_s);
            ESP_LOGW(TAG, "  1. board ran `pppd /dev/ttyS0 %d &`", CONFIG_GATEWAY_UART_BAUD);
            ESP_LOGW(TAG, "  2. wiring: our TX GPIO%d -> PA20, our RX GPIO%d <- PA27, GND to GND",
                     CONFIG_GATEWAY_UART_TX_GPIO, CONFIG_GATEWAY_UART_RX_GPIO);
            ESP_LOGW(TAG, "  3. if the board is dialling but nothing arrives here, set "
                          "GATEWAY_PPP_PASSIVE=n and rebuild");
        }

        if (idle_s >= CONFIG_GATEWAY_REARM_SEC) {
            ESP_LOGI(TAG, "re-arming the listener");
            esp_netif_action_start(s_ppp_netif, NULL, 0, NULL);
            esp_netif_action_connected(s_ppp_netif, NULL, 0, NULL);
            since_arm = 0;
        }
    }
}

/* --- entry point --------------------------------------------------------- */

static bool parse_ipv4(const char *text, esp_ip4_addr_t *out)
{
    /* esp_ip4_addr_t is its own struct in this IDF, not lwIP's ip4_addr_t, so
     * lwIP's ip4addr_aton() cannot write into one -- this is the matching
     * parser. */
    return esp_netif_str_to_ip4(text, out) == ESP_OK;
}

esp_err_t net_ppp_start(void)
{
    s_ppp_events = xEventGroupCreate();
    if (s_ppp_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* The transport has to exist before the interface is started: the first
     * LCP Configure-Request goes out through the transmit callback. */
    esp_err_t err = ppp_uart_init();
    if (err != ESP_OK) {
        return err;
    }

    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_PPP();
    base_cfg.if_desc = "ppp";

    const esp_netif_config_t netif_cfg = {
        .base   = &base_cfg,
        .driver = &s_driver_cfg,
        .stack  = ESP_NETIF_NETSTACK_DEFAULT_PPP,
    };

    s_ppp_netif = esp_netif_new(&netif_cfg);
    if (s_ppp_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return ESP_FAIL;
    }

    /* These have to be in place before the session starts: esp_netif_start_ppp()
     * copies them into the lwIP PCB and chooses between ppp_listen() and
     * ppp_connect() from ppp_passive. */
    esp_netif_ppp_config_t ppp_cfg = { 0 };
    ppp_cfg.ppp_phase_event_enabled = true;
    ppp_cfg.ppp_error_event_enabled = true;
    ppp_cfg.ppp_passive = CONFIG_GATEWAY_PPP_PASSIVE;

    if (!parse_ipv4(CONFIG_GATEWAY_PPP_OUR_IP, &ppp_cfg.ppp_our_ip4_addr)) {
        ESP_LOGE(TAG, "GATEWAY_PPP_OUR_IP is not an IPv4 address: \"%s\"", CONFIG_GATEWAY_PPP_OUR_IP);
        return ESP_ERR_INVALID_ARG;
    }
    if (!parse_ipv4(CONFIG_GATEWAY_PPP_PEER_IP, &ppp_cfg.ppp_their_ip4_addr)) {
        ESP_LOGE(TAG, "GATEWAY_PPP_PEER_IP is not an IPv4 address: \"%s\"", CONFIG_GATEWAY_PPP_PEER_IP);
        return ESP_ERR_INVALID_ARG;
    }
    if (CONFIG_GATEWAY_PPP_DNS[0] != '\0' && !parse_ipv4(CONFIG_GATEWAY_PPP_DNS, &ppp_cfg.ppp_dns1_addr)) {
        ESP_LOGW(TAG, "GATEWAY_PPP_DNS is not an IPv4 address: \"%s\" -- advertising none",
                 CONFIG_GATEWAY_PPP_DNS);
        ppp_cfg.ppp_dns1_addr.addr = IPADDR_ANY;
    }

    err = esp_netif_ppp_set_params(s_ppp_netif, &ppp_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_ppp_set_params failed: %s", esp_err_to_name(err));
        return err;
    }
    /* Authentication stays off: the board's pppd is built without PAP/CHAP and
     * ESP-IDF refuses this call unless one of them is compiled in, so there is
     * nothing to configure -- which is exactly the desired state. */

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, on_ppp_got_ip, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, on_ppp_lost_ip, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, on_ppp_status, NULL, NULL));

    if (xTaskCreate(ppp_rx_task, "ppp_rx", PPP_RX_TASK_STACK, NULL, PPP_RX_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the PPP receive task");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ppp_supervisor_task, "ppp_sup", PPP_SUP_TASK_STACK, NULL, PPP_SUP_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the PPP supervisor task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "listening on UART%d (TX GPIO%d, RX GPIO%d) at %d baud, %s",
             CONFIG_GATEWAY_UART_PORT, CONFIG_GATEWAY_UART_TX_GPIO, CONFIG_GATEWAY_UART_RX_GPIO,
             CONFIG_GATEWAY_UART_BAUD, CONFIG_GATEWAY_PPP_PASSIVE ? "passive" : "active");

    esp_netif_action_start(s_ppp_netif, NULL, 0, NULL);
    esp_netif_action_connected(s_ppp_netif, NULL, 0, NULL);
    return ESP_OK;
}

esp_netif_t *net_ppp_netif(void)
{
    return s_ppp_netif;
}

bool net_ppp_is_up(void)
{
    return s_ppp_events != NULL && (xEventGroupGetBits(s_ppp_events) & PPP_UP_BIT) != 0;
}
