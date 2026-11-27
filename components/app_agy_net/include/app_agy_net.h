// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "app_agy_ui.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start background Wi-Fi fallback sync using Vibe Mode shared credentials.
 * Safe when no Wi-Fi credentials are stored (continues in USB-only mode).
 */
esp_err_t app_agy_net_start(app_agy_ui_handle_t ui_ctrl);

/** Record recent USB Iris RPC traffic and optionally update saved Mac HTTP host. */
void app_agy_net_notify_usb_activity(const char *mac_host);

/** Wake the Wi-Fi task immediately when manual refresh is requested off-USB. */
void app_agy_net_request_refresh(void);

#ifdef __cplusplus
}
#endif
