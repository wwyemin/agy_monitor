// SPDX-License-Identifier: Apache-2.0

#include "app_agy_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_agy_proto.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mosaico_shared.h"
#include "nvs.h"

static const char *TAG = "agy_net";

#define AGY_NVS_NS "agy_cfg"
#define AGY_NVS_HOST_KEY "mac_host"
#define AGY_DISCOVER_PORT 18766U
#define AGY_STARTUP_USB_GRACE_MS 12000ULL
#define AGY_USB_ACTIVE_HOLD_MS 16000ULL
#define AGY_WIFI_RECONNECT_MS 2000ULL
#define AGY_WIFI_RETRY_MS 2500ULL
#define AGY_DISCOVER_COOLDOWN_MS 10000ULL
#define AGY_DEFAULT_INTERVAL_MS 10000ULL

static struct {
    portMUX_TYPE lock;
    app_agy_ui_handle_t ui_ctrl;
    TaskHandle_t task;

    bool wifi_started;
    volatile bool wifi_got_ip;
    volatile bool wifi_disconnected;
    esp_netif_ip_info_t ip_info;
    bool has_ip_info;
    uint64_t last_reconnect_try_ms;

    uint64_t last_usb_activity_ms;
    uint64_t next_wifi_sync_ms;
    uint64_t last_discover_ms;
    uint64_t auto_interval_ms;
    bool refresh_pending;
    bool pending_host_save;
    int consecutive_fails;

    char mac_host[32];
    char pending_mac_host[32];
    char ssid[33];
    char password[65];

    esp_iris_status_t iris_status;
    app_agy_snapshot_t snap_buf;
    char url_buf[96];
    char rx_buf[1024];
    char disc_buf[64];
} s_net = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
    .auto_interval_ms = AGY_DEFAULT_INTERVAL_MS,
};

static uint64_t uptime_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static bool is_usable_mac_host(const char *host)
{
    if (!host || host[0] == '\0') {
        return false;
    }
    if (strncmp(host, "127.", 4) == 0 ||
        strncmp(host, "169.254.", 8) == 0 ||
        strncmp(host, "198.18.", 7) == 0 ||
        strncmp(host, "198.19.", 7) == 0) {
        return false;
    }
    return true;
}

static void erase_saved_host(void)
{
    nvs_handle_t handle;
    if (nvs_open(AGY_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    (void)nvs_erase_key(handle, AGY_NVS_HOST_KEY);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

static void load_saved_host(void)
{
    nvs_handle_t handle;
    if (nvs_open(AGY_NVS_NS, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_net.mac_host);
    if (nvs_get_str(handle, AGY_NVS_HOST_KEY, s_net.mac_host, &len) != ESP_OK) {
        s_net.mac_host[0] = '\0';
    }
    nvs_close(handle);
    if (s_net.mac_host[0] != '\0' && !is_usable_mac_host(s_net.mac_host)) {
        ESP_LOGW(TAG, "Clearing unusable saved host: %s", s_net.mac_host);
        s_net.mac_host[0] = '\0';
        erase_saved_host();
    }
}

static void persist_host(const char *host)
{
    if (!is_usable_mac_host(host)) {
        return;
    }
    nvs_handle_t handle;
    if (nvs_open(AGY_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (nvs_set_str(handle, AGY_NVS_HOST_KEY, host) == ESP_OK) {
        (void)nvs_commit(handle);
    }
    nvs_close(handle);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            s_net.wifi_disconnected = false;
            (void)esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            const wifi_event_sta_disconnected_t *disc =
                (const wifi_event_sta_disconnected_t *)data;
            s_net.wifi_got_ip = false;
            s_net.has_ip_info = false;
            s_net.wifi_disconnected = true;
            s_net.last_reconnect_try_ms = uptime_ms();
            ESP_LOGW(TAG, "Wi-Fi disconnected reason=%u",
                     disc ? (unsigned)disc->reason : 0U);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        s_net.ip_info = event->ip_info;
        s_net.has_ip_info = true;
        s_net.wifi_got_ip = true;
        s_net.wifi_disconnected = false;
        s_net.next_wifi_sync_ms = 0;
        ESP_LOGI(TAG, "Wi-Fi ready ip=" IPSTR, IP2STR(&event->ip_info.ip));
        if (s_net.task != NULL) {
            xTaskNotifyGive(s_net.task);
        }
    }
}

static bool is_usb_preferred(uint64_t now_ms)
{
    uint64_t last_usb = 0;
    taskENTER_CRITICAL(&s_net.lock);
    last_usb = s_net.last_usb_activity_ms;
    taskEXIT_CRITICAL(&s_net.lock);

    if (last_usb > 0 && (now_ms - last_usb) < AGY_USB_ACTIVE_HOLD_MS) {
        return true;
    }

    if (esp_iris_get_status(&s_net.iris_status) == ESP_OK &&
        s_net.iris_status.link_connected &&
        s_net.iris_status.transport == ESP_IRIS_TRANSPORT_KIND_USB) {
        if (now_ms < AGY_STARTUP_USB_GRACE_MS) {
            return true;
        }
    }
    return false;
}

static bool discover_mac_host(void)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return false;
    }

    const int broadcast = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    const struct timeval tv = {.tv_sec = 0, .tv_usec = 800000};
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint32_t targets[2] = {htonl(INADDR_BROADCAST), 0U};
    size_t target_count = 1U;
    if (s_net.has_ip_info && s_net.ip_info.netmask.addr != 0U) {
        const uint32_t bcast =
            (s_net.ip_info.ip.addr & s_net.ip_info.netmask.addr) |
            (~s_net.ip_info.netmask.addr);
        if (bcast != htonl(INADDR_BROADCAST)) {
            targets[0] = bcast;
            targets[1] = htonl(INADDR_BROADCAST);
            target_count = 2U;
        }
    }

    static const char k_probe[] = "AGY_DISCOVER";
    for (int round = 0; round < 2; ++round) {
        for (size_t i = 0; i < target_count; ++i) {
            struct sockaddr_in dst = {0};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(AGY_DISCOVER_PORT);
            dst.sin_addr.s_addr = targets[i];
            (void)sendto(sock, k_probe, sizeof(k_probe) - 1U, 0,
                         (const struct sockaddr *)&dst, sizeof(dst));
        }

        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        const ssize_t recvd = recvfrom(sock, s_net.disc_buf, sizeof(s_net.disc_buf) - 1U, 0,
                                       (struct sockaddr *)&from, &from_len);
        if (recvd <= 0) {
            continue;
        }
        s_net.disc_buf[recvd] = '\0';

        unsigned port = 0;
        if (sscanf(s_net.disc_buf, "AGY_HOST %u", &port) != 1 || port == 0 || port > 65535U) {
            continue;
        }
        const char *ip_str = inet_ntoa(from.sin_addr);
        if (!is_usable_mac_host(ip_str)) {
            continue;
        }
        snprintf(s_net.mac_host, sizeof(s_net.mac_host), "%s:%u", ip_str, port);
        persist_host(s_net.mac_host);
        ESP_LOGI(TAG, "Discovered Mac companion at %s", s_net.mac_host);
        close(sock);
        return true;
    }

    close(sock);
    return false;
}

static bool fetch_wifi_snapshot(bool manual_refresh)
{
    if (!is_usable_mac_host(s_net.mac_host)) {
        return false;
    }

    snprintf(s_net.url_buf, sizeof(s_net.url_buf), "http://%s%s",
             s_net.mac_host, manual_refresh ? "/api/refresh" : "/api/status");

    const esp_http_client_config_t cfg = {
        .url = s_net.url_buf,
        .method = manual_refresh ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = manual_refresh ? 5500 : 4500,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return false;
    }

    bool ok = false;
    int status = -1;
    const esp_err_t open_err = esp_http_client_open(client, 0);
    if (open_err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status == 200) {
            int total = 0;
            while (total < (int)sizeof(s_net.rx_buf) - 1) {
                const int r = esp_http_client_read(
                    client, s_net.rx_buf + total, (int)sizeof(s_net.rx_buf) - 1 - total);
                if (r <= 0) {
                    break;
                }
                total += r;
            }
            s_net.rx_buf[total] = '\0';
            if (total > 0 && app_agy_proto_parse_sync(s_net.rx_buf, (size_t)total, &s_net.snap_buf)) {
                ok = true;
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!ok) {
        ESP_LOGW(TAG, "Wi-Fi HTTP sync failed: err=%s status=%d host=%s",
                 esp_err_to_name(open_err), status, s_net.mac_host);
    }
    return ok;
}

static void wifi_sync_task(void *arg)
{
    (void)arg;
    while (true) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        const uint64_t now = uptime_ms();

        bool save_host = false;
        taskENTER_CRITICAL(&s_net.lock);
        if (s_net.pending_host_save && is_usable_mac_host(s_net.pending_mac_host)) {
            snprintf(s_net.mac_host, sizeof(s_net.mac_host), "%s", s_net.pending_mac_host);
            s_net.pending_host_save = false;
            save_host = true;
        }
        taskEXIT_CRITICAL(&s_net.lock);
        if (save_host) {
            persist_host(s_net.mac_host);
        }

        if (!s_net.wifi_got_ip) {
            if (s_net.wifi_disconnected &&
                (now - s_net.last_reconnect_try_ms >= AGY_WIFI_RECONNECT_MS)) {
                s_net.last_reconnect_try_ms = now;
                (void)esp_wifi_connect();
            }
            continue;
        }

        if (is_usb_preferred(now)) {
            s_net.consecutive_fails = 0;
            s_net.next_wifi_sync_ms = 0;
            taskENTER_CRITICAL(&s_net.lock);
            s_net.refresh_pending = false;
            taskEXIT_CRITICAL(&s_net.lock);
            continue;
        }

        bool manual = false;
        uint64_t interval_ms = AGY_DEFAULT_INTERVAL_MS;
        taskENTER_CRITICAL(&s_net.lock);
        manual = s_net.refresh_pending;
        s_net.refresh_pending = false;
        interval_ms = s_net.auto_interval_ms;
        taskEXIT_CRITICAL(&s_net.lock);

        const bool due = (s_net.next_wifi_sync_ms == 0) || (now >= s_net.next_wifi_sync_ms);
        if (!manual && !due) {
            continue;
        }

        const bool need_discover =
            !is_usable_mac_host(s_net.mac_host) ||
            (s_net.consecutive_fails >= 2 &&
             (s_net.last_discover_ms == 0 || (now - s_net.last_discover_ms >= AGY_DISCOVER_COOLDOWN_MS)));
        if (need_discover) {
            s_net.last_discover_ms = now;
            (void)discover_mac_host();
        }

        if (fetch_wifi_snapshot(manual)) {
            if (s_net.consecutive_fails > 0) {
                ESP_LOGI(TAG, "Wi-Fi sync recovered host=%s", s_net.mac_host);
            }
            s_net.consecutive_fails = 0;
            if (s_net.snap_buf.auto_interval_sec >= 2) {
                interval_ms = (uint64_t)s_net.snap_buf.auto_interval_sec * 1000ULL;
                taskENTER_CRITICAL(&s_net.lock);
                s_net.auto_interval_ms = interval_ms;
                taskEXIT_CRITICAL(&s_net.lock);
            }
            const uint64_t done_ms = uptime_ms();
            s_net.next_wifi_sync_ms = done_ms + interval_ms;
            if (!is_usb_preferred(done_ms) && s_net.ui_ctrl != NULL) {
                app_agy_ui_submit_sync(s_net.ui_ctrl, &s_net.snap_buf, APP_AGY_LINK_WIFI);
            }
        } else {
            s_net.consecutive_fails++;
            s_net.next_wifi_sync_ms = uptime_ms() + AGY_WIFI_RETRY_MS;
        }
    }
}

esp_err_t app_agy_net_start(app_agy_ui_handle_t ui_ctrl)
{
    s_net.ui_ctrl = ui_ctrl;
    load_saved_host();

    const esp_err_t ssid_err = mosaico_shared_get("wifi_ssid", s_net.ssid, sizeof(s_net.ssid));
    if (ssid_err != ESP_OK || s_net.ssid[0] == '\0') {
        ESP_LOGW(TAG, "No shared Wi-Fi configured in Vibe Mode (%s); running in USB-only mode",
                 esp_err_to_name(ssid_err));
        return ESP_OK;
    }

    const esp_err_t pwd_err =
        mosaico_shared_get("wifi_password", s_net.password, sizeof(s_net.password));
    if (pwd_err != ESP_OK && pwd_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Shared Wi-Fi password read error (%s); treating as open",
                 esp_err_to_name(pwd_err));
        s_net.password[0] = '\0';
    }

    ESP_ERROR_CHECK(esp_netif_init());
    const esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        return loop_err;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    wifi_config_t cfg = {0};
    const size_t ssid_len = strnlen(s_net.ssid, sizeof(cfg.sta.ssid));
    const size_t pwd_len = strnlen(s_net.password, sizeof(cfg.sta.password));
    memcpy(cfg.sta.ssid, s_net.ssid, ssid_len);
    memcpy(cfg.sta.password, s_net.password, pwd_len);
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.listen_interval = 1;
    cfg.sta.threshold.authmode = (pwd_len > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Disable Wi-Fi modem sleep so low-latency LAN polls & UDP discovery never drop. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_ps(WIFI_PS_NONE));
    s_net.wifi_started = true;

    if (xTaskCreate(wifi_sync_task, "agy_wifi_sync", 6144, NULL, 4, &s_net.task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Dual USB + Wi-Fi sync enabled (SSID=%s, saved_host=%s)",
             s_net.ssid, s_net.mac_host[0] ? s_net.mac_host : "auto-discover");
    return ESP_OK;
}

void app_agy_net_notify_usb_activity(const char *mac_host)
{
    const uint64_t now = uptime_ms();
    taskENTER_CRITICAL(&s_net.lock);
    s_net.last_usb_activity_ms = now;
    if (is_usable_mac_host(mac_host) &&
        strncmp(s_net.mac_host, mac_host, sizeof(s_net.mac_host)) != 0) {
        snprintf(s_net.pending_mac_host, sizeof(s_net.pending_mac_host), "%s", mac_host);
        s_net.pending_host_save = true;
    }
    taskEXIT_CRITICAL(&s_net.lock);
}

void app_agy_net_request_refresh(void)
{
    if (!s_net.wifi_started || s_net.task == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_net.lock);
    s_net.refresh_pending = true;
    taskEXIT_CRITICAL(&s_net.lock);
    xTaskNotifyGive(s_net.task);
}
