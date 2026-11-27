// SPDX-License-Identifier: Apache-2.0

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_agy_ui.h"
#include "gsp_sim_bridge.h"

static struct {
    esp_gsp_handle_t ui;
    app_agy_ui_handle_t controller;
    void *demo_timer;
    uint32_t last_seen_seq;
    int pending_reply_ticks;
    int demo_quota;
    app_agy_link_t demo_link;
} backend;

static int32_t host_tz_offset_sec(void)
{
    const time_t now = time(NULL);
    struct tm local_tm;
    localtime_r(&now, &local_tm);
#if defined(__APPLE__) || defined(__linux__)
    return (int32_t)local_tm.tm_gmtoff;
#else
    return 28800;
#endif
}

static void push_demo_snapshot(uint32_t ack_seq, int quota_pct, const char *state_str,
                               app_agy_link_t link)
{
    app_agy_snapshot_t snap;
    app_agy_snapshot_init(&snap);
    snap.epoch_sec = (int64_t)time(NULL);
    snap.tz_offset_sec = host_tz_offset_sec();
    snap.quota_pct = quota_pct;
    snap.ctx_pct = 34;
    snap.active_sessions = 2;
    snap.auto_interval_sec = 10;
    snap.ack_refresh_seq = ack_seq;
    snprintf(snap.quota_window, sizeof(snap.quota_window), "%s", "5H 周期");
    snprintf(snap.quota_reset, sizeof(snap.quota_reset), "%s", "重置 3h 12m");
    snprintf(snap.quota_sub, sizeof(snap.quota_sub), "%s", "Pro · 额度正常");
    snprintf(snap.model, sizeof(snap.model), "%s", "Gemini 4 Neon (High)");
    snprintf(snap.state, sizeof(snap.state), "%s", state_str);
    snprintf(snap.workspace, sizeof(snap.workspace), "%s", "ESP-MOSAICO · main");
    snprintf(snap.mac_host, sizeof(snap.mac_host), "%s", "192.168.1.23:18765");
    snap.valid = true;
    app_agy_ui_submit_sync(backend.controller, &snap, link);
}

static void on_sim_tick(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)ui;
    (void)user_ctx;
    uint32_t seq = 0;
    bool syncing = false;
    app_agy_ui_get_poll_state(backend.controller, &seq, &syncing);

    if (seq > backend.last_seen_seq && backend.pending_reply_ticks == 0) {
        backend.pending_reply_ticks = 3;
    }
    if (backend.pending_reply_ticks > 0) {
        backend.pending_reply_ticks--;
        if (backend.pending_reply_ticks == 0) {
            backend.last_seen_seq = seq;
            backend.demo_quota = (backend.demo_quota > 10) ? (backend.demo_quota - 1) : 78;
            backend.demo_link = (backend.demo_link == APP_AGY_LINK_USB)
                                    ? APP_AGY_LINK_WIFI
                                    : APP_AGY_LINK_USB;
            push_demo_snapshot(seq, backend.demo_quota, "RUNNING", backend.demo_link);
        }
    }
}

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    backend.ui = ui;
    backend.demo_quota = 78;
    backend.demo_link = APP_AGY_LINK_USB;
    const esp_gsp_err_t err = app_agy_ui_create(ui, NULL, &backend.controller);
    if (err != ESP_GSP_OK) {
        return err;
    }
    push_demo_snapshot(0, backend.demo_quota, "RUNNING", backend.demo_link);
    backend.demo_timer = esp_gsp_timer_create(ui, 250U, on_sim_tick, NULL);
    return ESP_GSP_OK;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    if (backend.demo_timer) {
        (void)esp_gsp_timer_delete(ui, backend.demo_timer);
        backend.demo_timer = NULL;
    }
    (void)app_agy_ui_destroy(&backend.controller);
}
