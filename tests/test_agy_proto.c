// SPDX-License-Identifier: Apache-2.0

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_agy_proto.h"

static void test_parse_full_snapshot(void)
{
    const char *json =
        "{\"ts\":1795750328,\"tz\":28800,\"q_pct\":78,\"q_win\":\"5H 周期\","
        "\"q_rst\":\"重置 3h 12m\",\"q_sub\":\"Pro · 额度正常\","
        "\"model\":\"Gemini 4 Neon (High)\",\"state\":\"RUNNING\","
        "\"ctx\":34,\"sess\":2,\"ws\":\"ESP-MOSAICO · main\",\"host\":\"192.168.1.23:18765\",\"auto\":10,\"ack\":4}";
    app_agy_snapshot_t snap;
    assert(app_agy_proto_parse_sync(json, strlen(json), &snap));
    assert(snap.valid);
    assert(snap.epoch_sec == 1795750328LL);
    assert(snap.tz_offset_sec == 28800);
    assert(snap.quota_pct == 78);
    assert(snap.ctx_pct == 34);
    assert(snap.active_sessions == 2);
    assert(snap.auto_interval_sec == 10);
    assert(snap.ack_refresh_seq == 4U);
    assert(strcmp(snap.model, "Gemini 4 Neon (High)") == 0);
    assert(strcmp(snap.state, "RUNNING") == 0);
    assert(strcmp(snap.quota_window, "5H 周期") == 0);
    assert(strcmp(snap.quota_reset, "重置 3h 12m") == 0);
    assert(strcmp(snap.workspace, "ESP-MOSAICO · main") == 0);
    assert(strcmp(snap.mac_host, "192.168.1.23:18765") == 0);
}

static void test_parse_unknown_keys_and_escaped_strings(void)
{
    const char *json =
        "{\"extra\":{\"nested\":[1,2,3]},\"ts\":1700000000,\"tz\":28800,"
        "\"q_pct\":-1,\"model\":\"Claude \\\"Opus\\\" 5.5\",\"state\":\"IDLE\","
        "\"ctx\":-1,\"sess\":-1}";
    app_agy_snapshot_t snap;
    assert(app_agy_proto_parse_sync(json, strlen(json), &snap));
    assert(snap.quota_pct == -1);
    assert(snap.ctx_pct == -1);
    assert(snap.active_sessions == -1);
    assert(strcmp(snap.model, "Claude \"Opus\" 5.5") == 0);
}

static void test_parse_rejects_malformed(void)
{
    app_agy_snapshot_t snap;
    assert(!app_agy_proto_parse_sync("{}", 2, &snap));
    assert(!app_agy_proto_parse_sync("{bad", 4, &snap));
    assert(!app_agy_proto_parse_sync("{\"q_win\":\"only\"}", 16, &snap));
}

static void test_format_ack_and_clock(void)
{
    char ack[96];
    const int n = app_agy_proto_format_ack(ack, sizeof(ack), 7U, true);
    assert(n > 0);
    assert(strcmp(ack, "{\"ok\":true,\"refresh_seq\":7,\"syncing\":true}") == 0);

    char hhmmss[10];
    char md[16];
    /* 2026-11-27 11:20:05 UTC+8 -> UTC 2026-11-27 03:20:05 = 1795749605? Wait: let's test known 1970-01-01 00:00:00 + UTC+8 */
    app_agy_format_clock(172800LL, 28800, hhmmss, md);
    /* 1970-01-03 UTC 00:00:00 + 8h = 1970-01-03 08:00:00 (Saturday) */
    assert(strcmp(hhmmss, "08:00:00") == 0);
    assert(strcmp(md, "01-03 周六") == 0);

    /* Midnight rollover check: 1970-01-01 16:00:05 UTC + 8h = 1970-01-02 00:00:05 (Friday) */
    app_agy_format_clock(57605LL, 28800, hhmmss, md);
    assert(strcmp(hhmmss, "00:00:05") == 0);
    assert(strcmp(md, "01-02 周五") == 0);
}

int main(void)
{
    test_parse_full_snapshot();
    test_parse_unknown_keys_and_escaped_strings();
    test_parse_rejects_malformed();
    test_format_ack_and_clock();
    printf("test_agy_proto: ALL PASSED\n");
    return 0;
}
