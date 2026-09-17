/*
 * Wi-Fi station: the gateway's uplink.
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring up the Wi-Fi station and wait for an address.
 *
 * The station is set as the default netif on purpose.  It carries the
 * uplink, while the PPP interface carries the board; without this the
 * board's traffic would have no route off the device.  Note that the two
 * inherent configs already rank themselves this way (station route_prio 100,
 * PPP 20), so this makes the intent explicit rather than correcting anything.
 *
 * Call after esp_netif_init() and esp_event_loop_create_default().
 *
 * @param timeout_ms how long to wait for IP_EVENT_STA_GOT_IP
 * @return ESP_OK once the station has an address,
 *         ESP_ERR_TIMEOUT if it never arrived
 */
esp_err_t net_wifi_start(uint32_t timeout_ms);

/** The station netif, or NULL before net_wifi_start() succeeds. */
esp_netif_t *net_wifi_netif(void);

#ifdef __cplusplus
}
#endif
