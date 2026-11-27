// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_agy_proto.h"
#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct app_agy_ui *app_agy_ui_handle_t;

typedef struct {
    void (*on_vibrate)(uint32_t duration_ms, void *user_ctx);
    void (*on_refresh_requested)(void *user_ctx);
    uint64_t (*get_uptime_ms)(void *user_ctx);
    void *user_ctx;
} app_agy_ui_callbacks_t;

esp_gsp_err_t app_agy_ui_create(esp_gsp_handle_t ui,
                                const app_agy_ui_callbacks_t *callbacks,
                                app_agy_ui_handle_t *out_handle);

esp_gsp_err_t app_agy_ui_destroy(app_agy_ui_handle_t *handle);

/** Thread-safe: enqueue a parsed Mac sync snapshot and link type for UI rendering. */
void app_agy_ui_submit_sync(app_agy_ui_handle_t handle,
                            const app_agy_snapshot_t *snap,
                            app_agy_link_t link);

/** Thread-safe: trigger a manual refresh request (touch or AI hardware button). */
void app_agy_ui_trigger_refresh(app_agy_ui_handle_t handle);

/** Thread-safe: read current manual-refresh sequence & syncing flag for Mac polling. */
void app_agy_ui_get_poll_state(app_agy_ui_handle_t handle,
                               uint32_t *out_refresh_seq,
                               bool *out_syncing);

#ifdef __cplusplus
}
#endif
