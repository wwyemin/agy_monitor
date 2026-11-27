// SPDX-License-Identifier: Apache-2.0

#include "app_agy_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <time.h>
#endif

#define GSP_BUNDLE_ENABLE_RAW_IDS
#include "bundle_gsp.h"

#define UI_TIMER_PERIOD_MS 250U
#define SYNC_BUSY_TIMEOUT_MS 8000ULL
#define OFFLINE_MIN_TIMEOUT_MS 45000ULL
#define REFRESH_HAPTIC_MS 35U

typedef struct {
    char clock_label[16];
    char date_label[24];
    char state_label[16];
    bool dot_active;
    bool dot_offline;
    bool dot_warn;
    bool link_usb_vis;
    bool link_wifi_vis;
    bool link_none_vis;

    char quota_win_label[32];
    char quota_pct_label[16];
    char quota_reset_label[40];
    char quota_sub_label[48];
    int32_t quota_bar;

    char sessions_label[24];
    char model_label[56];
    char ctx_label[24];
    int32_t ctx_bar;
    char workspace_label[56];

    bool refresh_idle_vis;
    bool refresh_busy_vis;
    char auto_label[32];
    char sync_label[40];
} render_cache_t;

struct app_agy_ui {
    esp_gsp_handle_t ui;
    app_agy_ui_callbacks_t callbacks;
    void *timer;

#ifdef ESP_PLATFORM
    portMUX_TYPE lock;
#endif

    app_agy_snapshot_t pending_snap;
    app_agy_link_t pending_link;
    bool has_pending_snap;
    bool pending_refresh_req;

    app_agy_snapshot_t active;
    app_agy_link_t active_link;
    uint64_t last_sync_uptime_ms;
    bool has_synced_once;

    uint32_t refresh_seq;
    bool syncing;
    uint64_t sync_start_ms;
    bool sync_timed_out;

    render_cache_t cache;
};

static uint64_t read_uptime_ms(const struct app_agy_ui *ctx)
{
    if (ctx && ctx->callbacks.get_uptime_ms) {
        return ctx->callbacks.get_uptime_ms(ctx->callbacks.user_ctx);
    }
#ifdef ESP_PLATFORM
    return (uint64_t)(esp_timer_get_time() / 1000LL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
}

static void lock_state(struct app_agy_ui *ctx)
{
#ifdef ESP_PLATFORM
    taskENTER_CRITICAL(&ctx->lock);
#else
    (void)ctx;
#endif
}

static void unlock_state(struct app_agy_ui *ctx)
{
#ifdef ESP_PLATFORM
    taskEXIT_CRITICAL(&ctx->lock);
#else
    (void)ctx;
#endif
}

static void seed_render_cache(render_cache_t *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->clock_label, sizeof(c->clock_label), "%s", "--:--:--");
    snprintf(c->date_label, sizeof(c->date_label), "%s", "--/--");
    snprintf(c->state_label, sizeof(c->state_label), "%s", "OFFLINE");
    c->dot_active = false;
    c->dot_offline = true;
    c->dot_warn = false;
    c->link_usb_vis = false;
    c->link_wifi_vis = false;
    c->link_none_vis = true;

    snprintf(c->quota_win_label, sizeof(c->quota_win_label), "%s", "--");
    snprintf(c->quota_pct_label, sizeof(c->quota_pct_label), "%s", "--%");
    snprintf(c->quota_reset_label, sizeof(c->quota_reset_label), "%s", "--");
    snprintf(c->quota_sub_label, sizeof(c->quota_sub_label), "%s", "连接 USB 后自动同步");
    c->quota_bar = 0;

    snprintf(c->sessions_label, sizeof(c->sessions_label), "%s", "活跃会话 --");
    snprintf(c->model_label, sizeof(c->model_label), "%s", "等待同步...");
    snprintf(c->ctx_label, sizeof(c->ctx_label), "%s", "上下文 --%");
    c->ctx_bar = 0;
    snprintf(c->workspace_label, sizeof(c->workspace_label), "%s", "请运行 Mac 伴侣脚本");

    c->refresh_idle_vis = true;
    c->refresh_busy_vis = false;
    snprintf(c->auto_label, sizeof(c->auto_label), "%s", "每 10 秒自动同步");
    snprintf(c->sync_label, sizeof(c->sync_label), "%s", "等待 Mac 同步...");
}

static void sync_text(esp_gsp_handle_t ui, uint16_t bind, char *cached, size_t cap,
                      const char *next)
{
    if (strncmp(cached, next, cap) == 0) {
        return;
    }
    if (esp_gsp_set_text(ui, bind, next) == ESP_GSP_OK) {
        snprintf(cached, cap, "%s", next);
    }
}

static void sync_visible(esp_gsp_handle_t ui, uint16_t bind, bool *cached, bool next)
{
    if (*cached == next) {
        return;
    }
    if (esp_gsp_set_visible(ui, bind, next) == ESP_GSP_OK) {
        *cached = next;
    }
}

static void sync_value(esp_gsp_handle_t ui, uint16_t bind, int32_t *cached, int32_t next)
{
    if (*cached == next) {
        return;
    }
    if (esp_gsp_set_value(ui, bind, next) == ESP_GSP_OK) {
        *cached = next;
    }
}

static void render_ui(struct app_agy_ui *ctx, uint64_t now_ms)
{
    render_cache_t *c = &ctx->cache;
    const app_agy_snapshot_t *s = &ctx->active;
    esp_gsp_handle_t ui = ctx->ui;

    const uint64_t offline_ms =
        (s->auto_interval_sec > 0 && (uint64_t)s->auto_interval_sec * 4500ULL > OFFLINE_MIN_TIMEOUT_MS)
            ? (uint64_t)s->auto_interval_sec * 4500ULL
            : OFFLINE_MIN_TIMEOUT_MS;
    const bool is_offline =
        !ctx->has_synced_once || (now_ms - ctx->last_sync_uptime_ms > offline_ms);

    int64_t cur_epoch = 0;
    if (ctx->has_synced_once && s->epoch_sec > 0) {
        const uint64_t elapsed_sec = (now_ms - ctx->last_sync_uptime_ms) / 1000ULL;
        cur_epoch = s->epoch_sec + (int64_t)elapsed_sec;
    }

    char time_buf[10];
    char date_buf[16];
    app_agy_format_clock(cur_epoch, s->tz_offset_sec, time_buf, date_buf);
    sync_text(ui, GSP_MONITOR_BIND_CLOCK_LABEL, c->clock_label, sizeof(c->clock_label), time_buf);
    sync_text(ui, GSP_MONITOR_BIND_DATE_LABEL, c->date_label, sizeof(c->date_label), date_buf);

    const char *state_str = is_offline ? "OFFLINE" : (s->state[0] ? s->state : "IDLE");
    const bool is_error = (!is_offline && strcmp(state_str, "ERROR") == 0);
    const bool is_low_quota = (!is_offline && s->quota_pct >= 0 && s->quota_pct < 15);
    const bool show_warn = is_error || is_low_quota;
    const bool show_off = is_offline;
    const bool show_act = !show_off && !show_warn;

    sync_text(ui, GSP_MONITOR_BIND_STATE_LABEL, c->state_label, sizeof(c->state_label), state_str);
    sync_visible(ui, GSP_MONITOR_BIND_DOT_ACTIVE, &c->dot_active, show_act);
    sync_visible(ui, GSP_MONITOR_BIND_DOT_OFFLINE, &c->dot_offline, show_off);
    sync_visible(ui, GSP_MONITOR_BIND_DOT_WARN, &c->dot_warn, show_warn);

    const app_agy_link_t shown_link = is_offline ? APP_AGY_LINK_NONE : ctx->active_link;
    sync_visible(ui, GSP_MONITOR_BIND_LINK_USB_VIS, &c->link_usb_vis,
                 shown_link == APP_AGY_LINK_USB);
    sync_visible(ui, GSP_MONITOR_BIND_LINK_WIFI_VIS, &c->link_wifi_vis,
                 shown_link == APP_AGY_LINK_WIFI);
    sync_visible(ui, GSP_MONITOR_BIND_LINK_NONE_VIS, &c->link_none_vis,
                 shown_link == APP_AGY_LINK_NONE);

    char q_pct_buf[16];
    if (s->quota_pct >= 0) {
        snprintf(q_pct_buf, sizeof(q_pct_buf), "%d%%", (int)s->quota_pct);
    } else {
        snprintf(q_pct_buf, sizeof(q_pct_buf), "%s", "--%");
    }
    sync_text(ui, GSP_MONITOR_BIND_QUOTA_PCT_LABEL, c->quota_pct_label,
              sizeof(c->quota_pct_label), q_pct_buf);
    sync_value(ui, GSP_MONITOR_BIND_QUOTA_BAR, &c->quota_bar,
               (s->quota_pct >= 0) ? s->quota_pct : 0);
    sync_text(ui, GSP_MONITOR_BIND_QUOTA_WIN_LABEL, c->quota_win_label,
              sizeof(c->quota_win_label), s->quota_window);
    sync_text(ui, GSP_MONITOR_BIND_QUOTA_RESET_LABEL, c->quota_reset_label,
              sizeof(c->quota_reset_label), s->quota_reset);
    sync_text(ui, GSP_MONITOR_BIND_QUOTA_SUB_LABEL, c->quota_sub_label,
              sizeof(c->quota_sub_label), s->quota_sub);

    char sess_buf[24];
    if (s->active_sessions >= 0) {
        snprintf(sess_buf, sizeof(sess_buf), "活跃会话 %d", (int)s->active_sessions);
    } else {
        snprintf(sess_buf, sizeof(sess_buf), "%s", "活跃会话 --");
    }
    sync_text(ui, GSP_MONITOR_BIND_SESSIONS_LABEL, c->sessions_label,
              sizeof(c->sessions_label), sess_buf);
    sync_text(ui, GSP_MONITOR_BIND_MODEL_LABEL, c->model_label,
              sizeof(c->model_label), s->model);

    char ctx_buf[24];
    if (s->ctx_pct >= 0) {
        snprintf(ctx_buf, sizeof(ctx_buf), "上下文 %d%%", (int)s->ctx_pct);
    } else {
        snprintf(ctx_buf, sizeof(ctx_buf), "%s", "上下文 --%");
    }
    sync_text(ui, GSP_MONITOR_BIND_CTX_LABEL, c->ctx_label, sizeof(c->ctx_label), ctx_buf);
    sync_value(ui, GSP_MONITOR_BIND_CTX_BAR, &c->ctx_bar,
               (s->ctx_pct >= 0) ? s->ctx_pct : 0);
    sync_text(ui, GSP_MONITOR_BIND_WORKSPACE_LABEL, c->workspace_label,
              sizeof(c->workspace_label), s->workspace);

    sync_visible(ui, GSP_MONITOR_BIND_REFRESH_IDLE_VIS, &c->refresh_idle_vis, !ctx->syncing);
    sync_visible(ui, GSP_MONITOR_BIND_REFRESH_BUSY_VIS, &c->refresh_busy_vis, ctx->syncing);

    char auto_buf[32];
    snprintf(auto_buf, sizeof(auto_buf), "每 %d 秒自动同步", (int)s->auto_interval_sec);
    sync_text(ui, GSP_MONITOR_BIND_AUTO_LABEL, c->auto_label, sizeof(c->auto_label), auto_buf);

    char sync_buf[40];
    if (ctx->syncing) {
        snprintf(sync_buf, sizeof(sync_buf), "%s", "正在等待 Mac 响应...");
    } else if (ctx->sync_timed_out) {
        snprintf(sync_buf, sizeof(sync_buf), "%s", "同步超时，请检查 Mac");
    } else if (!ctx->has_synced_once) {
        snprintf(sync_buf, sizeof(sync_buf), "%s", "等待 Mac 同步...");
    } else if (is_offline) {
        snprintf(sync_buf, sizeof(sync_buf), "%s", "连接已断开，等待重连");
    } else {
        char sync_hm[10];
        app_agy_format_clock(s->epoch_sec, s->tz_offset_sec, sync_hm, NULL);
        const char *link_prefix = (ctx->active_link == APP_AGY_LINK_USB)
                                      ? "USB"
                                      : ((ctx->active_link == APP_AGY_LINK_WIFI) ? "WiFi" : "");
        if (link_prefix[0]) {
            snprintf(sync_buf, sizeof(sync_buf), "%s · 已同步 %s", link_prefix, sync_hm);
        } else {
            snprintf(sync_buf, sizeof(sync_buf), "已同步 %s", sync_hm);
        }
    }
    sync_text(ui, GSP_MONITOR_BIND_SYNC_LABEL, c->sync_label, sizeof(c->sync_label), sync_buf);
}

static void on_ui_timer(esp_gsp_handle_t ui, void *user_ctx)
{
    (void)ui;
    struct app_agy_ui *ctx = user_ctx;
    const uint64_t now_ms = read_uptime_ms(ctx);

    bool took_snap = false;
    bool took_refresh = false;

    lock_state(ctx);
    if (ctx->has_pending_snap) {
        ctx->active = ctx->pending_snap;
        ctx->active_link = ctx->pending_link;
        ctx->has_pending_snap = false;
        ctx->last_sync_uptime_ms = now_ms;
        ctx->has_synced_once = true;
        ctx->sync_timed_out = false;
        if (ctx->active.ack_refresh_seq >= ctx->refresh_seq || ctx->syncing) {
            ctx->syncing = false;
        }
        took_snap = true;
    }
    if (ctx->pending_refresh_req) {
        ctx->pending_refresh_req = false;
        ctx->syncing = true;
        ctx->sync_start_ms = now_ms;
        ctx->sync_timed_out = false;
        took_refresh = true;
    }
    unlock_state(ctx);

    if (ctx->syncing && !took_snap && (now_ms - ctx->sync_start_ms >= SYNC_BUSY_TIMEOUT_MS)) {
        lock_state(ctx);
        ctx->syncing = false;
        ctx->sync_timed_out = true;
        unlock_state(ctx);
    }

    (void)took_refresh;
    render_ui(ctx, now_ms);
}

static void on_ui_event(esp_gsp_handle_t ui, const esp_gsp_event_t *event, void *user_ctx)
{
    (void)ui;
    struct app_agy_ui *ctx = user_ctx;
    if (!ctx || !event) {
        return;
    }
    if (gsp_monitor_event_is_refresh_now(event)) {
        app_agy_ui_trigger_refresh(ctx);
    }
}

esp_gsp_err_t app_agy_ui_create(esp_gsp_handle_t ui,
                                const app_agy_ui_callbacks_t *callbacks,
                                app_agy_ui_handle_t *out_handle)
{
    if (!ui || !out_handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    struct app_agy_ui *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_GSP_ERR_NO_MEM;
    }
    ctx->ui = ui;
    if (callbacks) {
        ctx->callbacks = *callbacks;
    }
#ifdef ESP_PLATFORM
    portMUX_INITIALIZE(&ctx->lock);
#endif
    app_agy_snapshot_init(&ctx->active);
    seed_render_cache(&ctx->cache);

    ctx->timer = esp_gsp_timer_create(ui, UI_TIMER_PERIOD_MS, on_ui_timer, ctx);
    if (!ctx->timer) {
        free(ctx);
        return ESP_GSP_ERR_NO_MEM;
    }

    const esp_gsp_err_t err = esp_gsp_on_event(ui, on_ui_event, ctx);
    if (err != ESP_GSP_OK) {
        (void)esp_gsp_timer_delete(ui, ctx->timer);
        free(ctx);
        return err;
    }

    render_ui(ctx, read_uptime_ms(ctx));
    *out_handle = ctx;
    return ESP_GSP_OK;
}

esp_gsp_err_t app_agy_ui_destroy(app_agy_ui_handle_t *handle)
{
    if (!handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct app_agy_ui *ctx = *handle;
    if (!ctx) {
        return ESP_GSP_OK;
    }
    (void)esp_gsp_on_event(ctx->ui, NULL, NULL);
    if (ctx->timer) {
        (void)esp_gsp_timer_delete(ctx->ui, ctx->timer);
    }
    free(ctx);
    *handle = NULL;
    return ESP_GSP_OK;
}

void app_agy_ui_submit_sync(app_agy_ui_handle_t handle,
                            const app_agy_snapshot_t *snap,
                            app_agy_link_t link)
{
    if (!handle || !snap || !snap->valid) {
        return;
    }
    lock_state(handle);
    handle->pending_snap = *snap;
    handle->pending_link = link;
    handle->has_pending_snap = true;
    unlock_state(handle);
}

void app_agy_ui_trigger_refresh(app_agy_ui_handle_t handle)
{
    if (!handle) {
        return;
    }
    lock_state(handle);
    handle->refresh_seq++;
    handle->pending_refresh_req = true;
    unlock_state(handle);

    if (handle->callbacks.on_refresh_requested) {
        handle->callbacks.on_refresh_requested(handle->callbacks.user_ctx);
    }
    if (handle->callbacks.on_vibrate) {
        handle->callbacks.on_vibrate(REFRESH_HAPTIC_MS, handle->callbacks.user_ctx);
    }
}

void app_agy_ui_get_poll_state(app_agy_ui_handle_t handle,
                               uint32_t *out_refresh_seq,
                               bool *out_syncing)
{
    if (!handle) {
        if (out_refresh_seq) {
            *out_refresh_seq = 0;
        }
        if (out_syncing) {
            *out_syncing = false;
        }
        return;
    }
    lock_state(handle);
    const uint32_t seq = handle->refresh_seq;
    const bool syncing = handle->syncing || handle->pending_refresh_req;
    unlock_state(handle);

    if (out_refresh_seq) {
        *out_refresh_seq = seq;
    }
    if (out_syncing) {
        *out_syncing = syncing;
    }
}
