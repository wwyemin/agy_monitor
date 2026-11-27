// SPDX-License-Identifier: Apache-2.0

#include <string.h>

#include "app_agy_net.h"
#include "app_agy_proto.h"
#include "app_agy_ui.h"
#include "app_display.h"
#include "bsp/esp_mosaico.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "agy_monitor";

static struct {
    esp_gsp_handle_t ui;
    app_agy_ui_handle_t ui_ctrl;
    button_handle_t buttons[BSP_BUTTON_NUM];
    esp_timer_handle_t haptic_timer;
    bool motor_ready;
    SemaphoreHandle_t rpc_lock;
    app_agy_snapshot_t rpc_scratch;
} s_app;

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void haptic_off_cb(void *arg)
{
    (void)arg;
    if (s_app.motor_ready) {
        (void)bsp_motor_set(false);
    }
}

static void init_haptic(void)
{
    if (bsp_motor_init() != ESP_OK) {
        ESP_LOGW(TAG, "Haptic motor unavailable; visual feedback only");
        return;
    }
    (void)bsp_motor_set(false);
    const esp_timer_create_args_t args = {
        .callback = haptic_off_cb,
        .name = "agy_haptic",
    };
    if (esp_timer_create(&args, &s_app.haptic_timer) == ESP_OK) {
        s_app.motor_ready = true;
    }
}

static void on_ui_vibrate(uint32_t duration_ms, void *ctx)
{
    (void)ctx;
    if (!s_app.motor_ready || s_app.haptic_timer == NULL || duration_ms == 0U) {
        return;
    }
    (void)esp_timer_stop(s_app.haptic_timer);
    (void)bsp_motor_set(true);
    (void)esp_timer_start_once(s_app.haptic_timer, (uint64_t)duration_ms * 1000ULL);
}

static void on_ui_refresh_requested(void *ctx)
{
    (void)ctx;
    app_agy_net_request_refresh();
}

static uint64_t on_get_uptime_ms(void *ctx)
{
    (void)ctx;
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static esp_err_t rpc_sync_handler(const esp_iris_rpc_request_t *request,
                                  uint8_t *response,
                                  size_t response_capacity,
                                  size_t *response_size,
                                  void *user_ctx)
{
    (void)user_ctx;
    if (!request || !response || !response_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (request->payload == NULL || request->payload_size == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_app.rpc_lock == NULL ||
        xSemaphoreTake(s_app.rpc_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const bool parsed = app_agy_proto_parse_sync(
        (const char *)request->payload, request->payload_size, &s_app.rpc_scratch);
    if (parsed) {
        app_agy_net_notify_usb_activity(s_app.rpc_scratch.mac_host);
        if (s_app.ui_ctrl != NULL) {
            app_agy_ui_submit_sync(s_app.ui_ctrl, &s_app.rpc_scratch, APP_AGY_LINK_USB);
        }
    }
    xSemaphoreGive(s_app.rpc_lock);

    if (!parsed) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t refresh_seq = 0;
    bool syncing = false;
    app_agy_ui_get_poll_state(s_app.ui_ctrl, &refresh_seq, &syncing);

    const int n = app_agy_proto_format_ack(
        (char *)response, response_capacity, refresh_seq, syncing);
    if (n < 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    *response_size = (size_t)n;
    return ESP_OK;
}

static esp_err_t rpc_poll_handler(const esp_iris_rpc_request_t *request,
                                  uint8_t *response,
                                  size_t response_capacity,
                                  size_t *response_size,
                                  void *user_ctx)
{
    (void)request;
    (void)user_ctx;
    if (!response || !response_size) {
        return ESP_ERR_INVALID_ARG;
    }

    app_agy_net_notify_usb_activity(NULL);

    uint32_t refresh_seq = 0;
    bool syncing = false;
    app_agy_ui_get_poll_state(s_app.ui_ctrl, &refresh_seq, &syncing);

    const int n = app_agy_proto_format_ack(
        (char *)response, response_capacity, refresh_seq, syncing);
    if (n < 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    *response_size = (size_t)n;
    return ESP_OK;
}

static void on_ai_button_click(void *button_handle, void *user_data)
{
    (void)button_handle;
    (void)user_data;
    if (s_app.ui_ctrl != NULL) {
        app_agy_ui_trigger_refresh(s_app.ui_ctrl);
    }
}

static void init_buttons(void)
{
    int count = 0;
    const esp_err_t err = bsp_iot_button_create(s_app.buttons, &count, BSP_BUTTON_NUM);
    if (err == ESP_OK && count > 0 && s_app.buttons[BSP_BUTTON_AI] != NULL) {
        (void)iot_button_register_cb(s_app.buttons[BSP_BUTTON_AI], BUTTON_SINGLE_CLICK, NULL,
                                     on_ai_button_click, NULL);
    } else {
        ESP_LOGW(TAG, "AI button unavailable (%s); touch refresh remains active",
                 esp_err_to_name(err));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    init_nvs();
    s_app.rpc_lock = xSemaphoreCreateMutex();

    /* Register custom RPC endpoints before starting ESP-Iris/OTA support. */
    ESP_ERROR_CHECK(esp_iris_rpc_register(APP_AGY_RPC_SERVICE_ID, APP_AGY_RPC_METHOD_SYNC,
                                          rpc_sync_handler, NULL));
    ESP_ERROR_CHECK(esp_iris_rpc_register(APP_AGY_RPC_SERVICE_ID, APP_AGY_RPC_METHOD_POLL,
                                          rpc_poll_handler, NULL));
    /* Recovery must remain reachable even when peripherals fail to start. */
    iris_ota_support_start();

    init_haptic();

    esp_gsp_config_t ui_cfg;
    const esp_err_t bundle_err = ui_bundle_open(&ui_cfg);
    if (bundle_err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (%s); Recovery RPC remains active",
                 esp_err_to_name(bundle_err));
        return;
    }

    esp_gsp_esp_lcd_config_t display_cfg;
    const esp_err_t disp_err = app_display_init(&display_cfg);
    if (disp_err != ESP_OK) {
        ESP_LOGE(TAG, "Display unavailable (%s); Recovery RPC remains active",
                 esp_err_to_name(disp_err));
        return;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(iris_screen_mirror_init());
    if (esp_gsp_esp_lcd_start(&ui_cfg, &display_cfg, &s_app.ui) == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(iris_screen_mirror_attach(s_app.ui));

        const app_agy_ui_callbacks_t cbs = {
            .on_vibrate = on_ui_vibrate,
            .on_refresh_requested = on_ui_refresh_requested,
            .get_uptime_ms = on_get_uptime_ms,
            .user_ctx = NULL,
        };
        const esp_gsp_err_t ui_err = app_agy_ui_create(s_app.ui, &cbs, &s_app.ui_ctrl);
        if (ui_err != ESP_GSP_OK) {
            ESP_LOGE(TAG, "agy_monitor UI create failed (%d)", (int)ui_err);
        }
    }

    init_buttons();
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_agy_net_start(s_app.ui_ctrl));
    ESP_LOGI(TAG, "agy_monitor ready (RPC service 0x%04x + Wi-Fi failover)",
             (unsigned)APP_AGY_RPC_SERVICE_ID);
}
