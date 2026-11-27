// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Custom ESP-Iris RPC service ID ("AG"). */
#define APP_AGY_RPC_SERVICE_ID 0x4147U
/** Push latest Antigravity CLI usage, model, status, and Mac clock snapshot. */
#define APP_AGY_RPC_METHOD_SYNC 1U
/** Poll device manual-refresh sequence and syncing flag. */
#define APP_AGY_RPC_METHOD_POLL 2U

typedef enum {
    APP_AGY_LINK_NONE = 0,
    APP_AGY_LINK_USB = 1,
    APP_AGY_LINK_WIFI = 2,
} app_agy_link_t;

typedef struct {
    int64_t epoch_sec;
    int32_t tz_offset_sec;
    int32_t quota_pct;
    int32_t ctx_pct;
    int32_t active_sessions;
    int32_t auto_interval_sec;
    uint32_t ack_refresh_seq;
    char quota_window[28];
    char quota_reset[32];
    char quota_sub[40];
    char model[48];
    char state[16];
    char workspace[48];
    char mac_host[32];
    bool valid;
} app_agy_snapshot_t;

/** Initialize snapshot fields to safe disconnected defaults. */
void app_agy_snapshot_init(app_agy_snapshot_t *snap);

/**
 * Parse a flat UTF-8 JSON sync payload received from the Mac companion.
 * Unknown keys are ignored safely; missing optional fields stay at defaults.
 */
bool app_agy_proto_parse_sync(const char *json, size_t len, app_agy_snapshot_t *out);

/** Format a compact JSON RPC response carrying refresh_seq and syncing state. */
int app_agy_proto_format_ack(char *out, size_t cap, uint32_t refresh_seq, bool syncing);

/**
 * Format local clock strings from Unix epoch seconds and UTC offset seconds
 * using pure integer calendar math (no libc timezone state dependency).
 */
void app_agy_format_clock(int64_t epoch_sec, int32_t tz_offset_sec,
                          char time_hhmmss[10], char date_md_wday[16]);

#ifdef __cplusplus
}
#endif
