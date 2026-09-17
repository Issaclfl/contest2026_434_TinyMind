/*
 * PPP server + NAPT: the board dials in on a UART, we NAT it out via Wi-Fi.
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create the PPP interface, attach it to the configured UART and bring it up
 * as a server: we keep GATEWAY_PPP_OUR_IP, hand GATEWAY_PPP_PEER_IP to the
 * board, and NAT whatever the board sends out through the Wi-Fi station.
 *
 * Call after net_wifi_start(): the station has to hold an address before it
 * can serve as the uplink.  Returns as soon as listening has started -- the
 * board may take seconds to dial in, and a supervisor task logs progress.
 */
esp_err_t net_ppp_start(void);

/** The PPP netif, or NULL before net_ppp_start(). */
esp_netif_t *net_ppp_netif(void);

/** True once IPCP has completed and the board has been given an address. */
bool net_ppp_is_up(void);

#ifdef __cplusplus
}
#endif
