// SPDX-License-Identifier: Apache-2.0

#include "app_agy_proto.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void app_agy_snapshot_init(app_agy_snapshot_t *snap)
{
    if (!snap) {
        return;
    }
    memset(snap, 0, sizeof(*snap));
    snap->quota_pct = -1;
    snap->ctx_pct = -1;
    snap->active_sessions = -1;
    snap->auto_interval_sec = 10;
    snprintf(snap->state, sizeof(snap->state), "%s", "OFFLINE");
    snprintf(snap->model, sizeof(snap->model), "%s", "等待同步...");
    snprintf(snap->quota_window, sizeof(snap->quota_window), "%s", "--");
    snprintf(snap->quota_reset, sizeof(snap->quota_reset), "%s", "--");
    snprintf(snap->quota_sub, sizeof(snap->quota_sub), "%s", "连接 USB 后自动同步");
    snprintf(snap->workspace, sizeof(snap->workspace), "%s", "请运行 Mac 伴侣脚本");
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && isspace((unsigned char)*p)) {
        ++p;
    }
    return p;
}

static bool parse_json_string(const char **cursor, const char *end, char *out, size_t cap)
{
    const char *p = skip_ws(*cursor, end);
    if (p >= end || *p != '"') {
        return false;
    }
    ++p;
    size_t n = 0;
    while (p < end) {
        char c = *p++;
        if (c == '"') {
            if (out && cap > 0) {
                out[n] = '\0';
            }
            *cursor = p;
            return true;
        }
        if (c == '\\') {
            if (p >= end) {
                return false;
            }
            char esc = *p++;
            switch (esc) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = ' '; break;
            case 'r': c = ' '; break;
            case 't': c = ' '; break;
            case 'u':
                /* Skip 4 hex digits safely without failing the whole frame. */
                for (int i = 0; i < 4 && p < end; ++i, ++p) {
                }
                c = '?';
                break;
            default:
                c = esc;
                break;
            }
        }
        if (out && cap > 1 && n < cap - 1) {
            out[n++] = c;
        }
    }
    return false;
}

static bool parse_json_int64(const char **cursor, const char *end, int64_t *out)
{
    const char *p = skip_ws(*cursor, end);
    if (p >= end) {
        return false;
    }
    bool neg = false;
    if (*p == '-') {
        neg = true;
        ++p;
    }
    if (p >= end || !isdigit((unsigned char)*p)) {
        return false;
    }
    uint64_t val = 0;
    while (p < end && isdigit((unsigned char)*p)) {
        const uint64_t digit = (uint64_t)(*p - '0');
        if (val <= (UINT64_MAX - digit) / 10ULL) {
            val = val * 10ULL + digit;
        }
        ++p;
    }
    /* Accept trailing fractional part on numbers by skipping it. */
    if (p < end && *p == '.') {
        ++p;
        while (p < end && isdigit((unsigned char)*p)) {
            ++p;
        }
    }
    *out = neg ? -(int64_t)val : (int64_t)val;
    *cursor = p;
    return true;
}

static bool skip_json_value(const char **cursor, const char *end)
{
    const char *p = skip_ws(*cursor, end);
    if (p >= end) {
        return false;
    }
    if (*p == '"') {
        return parse_json_string(cursor, end, NULL, 0);
    }
    if (*p == '-' || isdigit((unsigned char)*p)) {
        int64_t dummy = 0;
        return parse_json_int64(cursor, end, &dummy);
    }
    if (*p == '{' || *p == '[') {
        const char open = *p++;
        const char close = (open == '{') ? '}' : ']';
        int depth = 1;
        while (p < end && depth > 0) {
            if (*p == '"') {
                if (!parse_json_string(&p, end, NULL, 0)) {
                    return false;
                }
                continue;
            }
            if (*p == open) {
                ++depth;
            } else if (*p == close) {
                --depth;
            }
            ++p;
        }
        if (depth != 0) {
            return false;
        }
        *cursor = p;
        return true;
    }
    while (p < end && *p != ',' && *p != '}' && !isspace((unsigned char)*p)) {
        ++p;
    }
    if (p == *cursor) {
        return false;
    }
    *cursor = p;
    return true;
}

static int32_t clamp_i32(int64_t v, int32_t lo, int32_t hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return (int32_t)v;
}

bool app_agy_proto_parse_sync(const char *json, size_t len, app_agy_snapshot_t *out)
{
    if (!json || !out || len == 0) {
        return false;
    }
    app_agy_snapshot_init(out);

    const char *p = json;
    const char *const end = json + len;
    p = skip_ws(p, end);
    if (p >= end || *p != '{') {
        return false;
    }
    ++p;
    p = skip_ws(p, end);
    if (p < end && *p == '}') {
        return false;
    }

    char key[32];
    bool has_core = false;
    while (p < end) {
        if (!parse_json_string(&p, end, key, sizeof(key))) {
            app_agy_snapshot_init(out);
            return false;
        }
        p = skip_ws(p, end);
        if (p >= end || *p != ':') {
            app_agy_snapshot_init(out);
            return false;
        }
        ++p;
        p = skip_ws(p, end);
        if (p >= end) {
            app_agy_snapshot_init(out);
            return false;
        }

        if (strcmp(key, "ts") == 0) {
            int64_t v = 0;
            if (!parse_json_int64(&p, end, &v)) {
                app_agy_snapshot_init(out);
                return false;
            }
            out->epoch_sec = v;
            has_core = true;
        } else if (strcmp(key, "tz") == 0) {
            int64_t v = 0;
            if (!parse_json_int64(&p, end, &v)) {
                app_agy_snapshot_init(out);
                return false;
            }
            out->tz_offset_sec = clamp_i32(v, -43200, 50400);
        } else if (strcmp(key, "q_pct") == 0) {
            int64_t v = -1;
            if (!parse_json_int64(&p, end, &v)) {
                app_agy_snapshot_init(out);
                return false;
            }
            out->quota_pct = (v < 0) ? -1 : clamp_i32(v, 0, 100);
        } else if (strcmp(key, "ctx") == 0) {
            int64_t v = -1;
            if (!parse_json_int64(&p, end, &v)) {
                app_agy_snapshot_init(out);
                return false;
            }
            out->ctx_pct = (v < 0) ? -1 : clamp_i32(v, 0, 100);
        } else if (strcmp(key, "sess") == 0) {
            int64_t v = -1;
            if (!parse_json_int64(&p, end, &v)) {
                app_agy_snapshot_init(out);
                return false;
            }
            out->active_sessions = (v < 0) ? -1 : clamp_i32(v, 0, 999);
        } else if (strcmp(key, "auto") == 0) {
            int64_t v = 10;
            if (!parse_json_int64(&p, end, &v)) {
                app_agy_snapshot_init(out);
                return false;
            }
            out->auto_interval_sec = clamp_i32(v, 2, 3600);
        } else if (strcmp(key, "ack") == 0) {
            int64_t v = 0;
            if (!parse_json_int64(&p, end, &v)) {
                app_agy_snapshot_init(out);
                return false;
            }
            out->ack_refresh_seq = (v < 0) ? 0U : (uint32_t)v;
        } else if (strcmp(key, "q_win") == 0) {
            if (*p == '"') {
                if (!parse_json_string(&p, end, out->quota_window, sizeof(out->quota_window))) {
                    app_agy_snapshot_init(out);
                    return false;
                }
            } else if (!skip_json_value(&p, end)) {
                app_agy_snapshot_init(out);
                return false;
            }
        } else if (strcmp(key, "q_rst") == 0) {
            if (*p == '"') {
                if (!parse_json_string(&p, end, out->quota_reset, sizeof(out->quota_reset))) {
                    app_agy_snapshot_init(out);
                    return false;
                }
            } else if (!skip_json_value(&p, end)) {
                app_agy_snapshot_init(out);
                return false;
            }
        } else if (strcmp(key, "q_sub") == 0) {
            if (*p == '"') {
                if (!parse_json_string(&p, end, out->quota_sub, sizeof(out->quota_sub))) {
                    app_agy_snapshot_init(out);
                    return false;
                }
            } else if (!skip_json_value(&p, end)) {
                app_agy_snapshot_init(out);
                return false;
            }
        } else if (strcmp(key, "model") == 0) {
            if (*p == '"') {
                if (!parse_json_string(&p, end, out->model, sizeof(out->model))) {
                    app_agy_snapshot_init(out);
                    return false;
                }
                has_core = true;
            } else if (!skip_json_value(&p, end)) {
                app_agy_snapshot_init(out);
                return false;
            }
        } else if (strcmp(key, "state") == 0) {
            if (*p == '"') {
                if (!parse_json_string(&p, end, out->state, sizeof(out->state))) {
                    app_agy_snapshot_init(out);
                    return false;
                }
                has_core = true;
            } else if (!skip_json_value(&p, end)) {
                app_agy_snapshot_init(out);
                return false;
            }
        } else if (strcmp(key, "ws") == 0) {
            if (*p == '"') {
                if (!parse_json_string(&p, end, out->workspace, sizeof(out->workspace))) {
                    app_agy_snapshot_init(out);
                    return false;
                }
            } else if (!skip_json_value(&p, end)) {
                app_agy_snapshot_init(out);
                return false;
            }
        } else if (strcmp(key, "host") == 0) {
            if (*p == '"') {
                if (!parse_json_string(&p, end, out->mac_host, sizeof(out->mac_host))) {
                    app_agy_snapshot_init(out);
                    return false;
                }
            } else if (!skip_json_value(&p, end)) {
                app_agy_snapshot_init(out);
                return false;
            }
        } else if (!skip_json_value(&p, end)) {
            app_agy_snapshot_init(out);
            return false;
        }

        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            ++p;
            p = skip_ws(p, end);
            continue;
        }
        if (p < end && *p == '}') {
            ++p;
            p = skip_ws(p, end);
            if (p != end || !has_core) {
                app_agy_snapshot_init(out);
                return false;
            }
            out->valid = true;
            return true;
        }
        app_agy_snapshot_init(out);
        return false;
    }
    app_agy_snapshot_init(out);
    return false;
}

int app_agy_proto_format_ack(char *out, size_t cap, uint32_t refresh_seq, bool syncing)
{
    if (!out || cap == 0) {
        return -1;
    }
    const int n = snprintf(out, cap, "{\"ok\":true,\"refresh_seq\":%lu,\"syncing\":%s}",
                           (unsigned long)refresh_seq, syncing ? "true" : "false");
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return n;
}

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) {
        --q;
    }
    return q;
}

void app_agy_format_clock(int64_t epoch_sec, int32_t tz_offset_sec,
                          char time_hhmmss[10], char date_md_wday[16])
{
    if (epoch_sec <= 0) {
        if (time_hhmmss) {
            snprintf(time_hhmmss, 10, "%s", "--:--:--");
        }
        if (date_md_wday) {
            snprintf(date_md_wday, 16, "%s", "--/--");
        }
        return;
    }

    const int64_t local_sec = epoch_sec + (int64_t)tz_offset_sec;
    const int64_t days = floor_div(local_sec, 86400LL);
    int64_t rem = local_sec - days * 86400LL;
    const unsigned hour = (unsigned)(rem / 3600LL);
    rem %= 3600LL;
    const unsigned minute = (unsigned)(rem / 60LL);
    const unsigned second = (unsigned)(rem % 60LL);

    if (time_hhmmss) {
        snprintf(time_hhmmss, 10, "%02u:%02u:%02u", hour, minute, second);
    }

    if (date_md_wday) {
        /* 1970-01-01 was Thursday (index 4 in Sun=0..Sat=6). */
        int64_t wday = (days + 4LL) % 7LL;
        if (wday < 0) {
            wday += 7LL;
        }
        static const char *const k_weekdays[7] = {
            "周日", "周一", "周二", "周三", "周四", "周五", "周六",
        };

        /* Howard Hinnant's civil-from-days algorithm. */
        const int64_t z = days + 719468LL;
        const int64_t era = (z >= 0 ? z : z - 146096LL) / 146097LL;
        const unsigned doe = (unsigned)(z - era * 146097LL);
        const unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
        const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
        const unsigned mp = (5U * doy + 2U) / 153U;
        const unsigned day = doy - (153U * mp + 2U) / 5U + 1U;
        const unsigned month = mp + (mp < 10U ? 3U : (unsigned)-9);
        snprintf(date_md_wday, 16, "%02u-%02u %s", month, day, k_weekdays[wday]);
    }
}
