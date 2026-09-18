/*
 * Cloud LLM route: the gateway decides per request whether the question goes
 * to the cloud model or to the local one.
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the cloud route with the local model's HTTP server.
 *
 * From here on the model endpoint doubles as a routing point: a request whose
 * model name says "local" (or names the local model) is answered locally, and
 * every other request tries the cloud first, falling back to the local model
 * when the uplink is gone or the cloud call fails.  The board never sees any
 * of this -- for it the endpoint stays one address.
 *
 * Safe to call when no cloud is configured: the route then counts requests
 * and answers locally, which is also the whole story when the key is empty.
 */
void net_cloud_start(void);

/** True once an API key is configured (never logs or serves the key itself). */
bool net_cloud_configured(void);

#ifdef __cplusplus
}
#endif
