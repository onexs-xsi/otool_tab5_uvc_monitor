/*
 * SPDX-FileCopyrightText: 2026 onexs-xsi
 *
 * SPDX-License-Identifier: MIT
 */

// UVC 应用流程：USB/UVC/UAC 初始化、热插拔状态机和 REINIT。
#include "app_internal.h"

#include <atomic>

static std::atomic<bool> s_reinit_in_flight{false};

static const TickType_t kUiUsbPollTicks = pdMS_TO_TICKS(200);
static const size_t kUsbAddrScanMax = 16;

static bool usb_addr_present(uint8_t dev_addr)
{
    if (dev_addr == 0) return false;

    uint8_t dev_addr_list[kUsbAddrScanMax] = {};
    int actual_count = 0;
    esp_err_t err = usb_host_device_addr_list_fill(sizeof(dev_addr_list), dev_addr_list, &actual_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "usb addr scan failed: %s", esp_err_to_name(err));
        return false;
    }

    if (actual_count < 0) actual_count = 0;
    if (actual_count > (int)kUsbAddrScanMax) actual_count = (int)kUsbAddrScanMax;
    for (int i = 0; i < actual_count; ++i) {
        if (dev_addr_list[i] == dev_addr) return true;
    }
    return false;
}

static void app_reset_connected_device_state(app_t *a)
{
    if (!a) return;

    const uint8_t lost_addr = a->dev_addr;
    if (a->uac_addr == lost_addr) {
        a->uac_rx_available = false;
        a->uac_addr = 0;
        a->uac_iface_num = 0;
    }
    if (a->uac_tx_addr == lost_addr) {
        a->uac_tx_available = false;
        a->uac_tx_addr = 0;
        a->uac_tx_iface_num = 0;
        a->uac_tx_start_attempted = false;
    }

    a->mode_count = 0;
    a->selected_mode = 0;
    a->dev_addr = 0;
    a->stream_idx = 0;
    a->stream_restart_requested = false;
    a->uvc_frame_size_override = 0;
    a->stream_restart_count = 0;
    a->uac_selected = false;
    a->adv_live_enabled = false;
    a->adv_touch_down = false;
    a->stream_lvgl_ui = false;
    cam_ctrl_init_table(a);
}

static void app_reset_stream_runtime_flags(app_t *a)
{
    if (!a) return;
    a->uac_selected = false;
    a->adv_live_enabled = false;
    a->adv_touch_down = false;
    a->stream_lvgl_ui = false;
}

static void app_prepare_stream_restart_state(app_t *a)
{
    if (!a) return;
    panel_clear(a);
    a->stream_restart_requested = false;
    a->uvc_frame_size_override = 0;
    a->stream_restart_count = 0;
}

static void app_close_stream_handle(app_t *a)
{
    if (!a) return;
    if (a->stream_hdl) {
        uvc_host_stream_stop(a->stream_hdl);
        drain_queue(a);
        uvc_host_stream_close(a->stream_hdl);
        a->stream_hdl = nullptr;
    } else {
        drain_queue(a);
    }
}

static void app_cleanup_after_stream(app_t *a)
{
    if (!a) return;
    stop_uac_stream(a);
    app_close_stream_handle(a);
    free_stream_resources(a);
    ui_destroy_stream_overlay(a);
    otool_lvgl_port_resume();
    a->state = APP_STATE_WAITING_USB;
}

static void app_refresh_connected_selection_ui(app_t *a, const char *status_text)
{
    if (!a) return;
    if (!otool_lvgl_port_lock(0)) return;

    ui_populate_mode_list(a);
    if (a->status_lbl) {
        const char *text = status_text;
        if (!text) {
            text = (a->mode_count > 0) ?
                       "Select mode and press START" :
                       "Camera connected, no compatible modes";
        }
        lv_label_set_text(a->status_lbl, text);
    }
    ui_update_uac_option(a);
    if (a->confirm_btn) {
        if (a->mode_count > 0 && !a->check_running)
            lv_obj_clear_state(a->confirm_btn, LV_STATE_DISABLED);
        else
            lv_obj_add_state(a->confirm_btn, LV_STATE_DISABLED);
    }
    if (a->check_btn) {
        if (a->mode_count > 0 && !a->check_running)
            lv_obj_clear_state(a->check_btn, LV_STATE_DISABLED);
        else
            lv_obj_add_state(a->check_btn, LV_STATE_DISABLED);
    }
    ui_update_info_panel(a);
    ui_update_advanced_button(a);
    otool_lvgl_port_unlock();
}

static void app_refresh_waiting_selection_ui(app_t *a, const char *status_text)
{
    if (!a) return;
    if (!otool_lvgl_port_lock(50)) return;

    ui_clear_mode_list(a);
    if (a->status_lbl) {
        lv_label_set_text(a->status_lbl, status_text ? status_text : "Waiting for USB camera...");
    }
    ui_update_uac_option(a);
    if (a->confirm_btn)
        lv_obj_add_state(a->confirm_btn, LV_STATE_DISABLED);
    if (a->check_btn)
        lv_obj_add_state(a->check_btn, LV_STATE_DISABLED);
    ui_update_info_panel(a);
    ui_update_advanced_button(a);
    otool_lvgl_port_unlock();
}

static void app_handle_selecting_device_lost(app_t *a, const char *status_text)
{
    if (!a) return;
    app_reset_connected_device_state(a);
    app_refresh_waiting_selection_ui(a, status_text ? status_text : "Camera disconnected, waiting...");
}

static void usb_reinit_task(void *arg)
{
    app_t *aa = (app_t *)arg;
    ESP_LOGI(TAG, "REINIT: powering USB5V OFF");
    esp_err_t err = app_usb5v_set(false, "user REINIT");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "REINIT: usb5v OFF returned %s", esp_err_to_name(err));
    }
    if (aa) ui_set_status(aa, "USB power OFF, waiting...");
    vTaskDelay(pdMS_TO_TICKS(800));
    ESP_LOGI(TAG, "REINIT: powering USB5V ON");
    err = app_usb5v_set(true, "user REINIT");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "REINIT: usb5v ON returned %s", esp_err_to_name(err));
    }
    if (aa) ui_set_status(aa, "USB power restored, waiting for camera...");
    s_reinit_in_flight.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void app_request_usb_reinit_from_user(app_t *a)
{
    if (!a) return;
    if (a->state == APP_STATE_STREAMING) {
        ESP_LOGW(TAG, "REINIT ignored: streaming in progress");
        ui_set_status(a, "Stop the stream before REINIT");
        return;
    }
    bool expected = false;
    if (!s_reinit_in_flight.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ESP_LOGW(TAG, "REINIT already in progress");
        return;
    }
    // 主动通知状态机：让选择面板及时清空模式列表。
    if (a->events) xEventGroupSetBits(a->events, EV_DEV_LOST | EV_REINIT_REQUESTED);
    BaseType_t ok = xTaskCreatePinnedToCore(usb_reinit_task, "usb_reinit", 4096, a,
                                            5, nullptr, tskNO_AFFINITY);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "REINIT: task create failed");
        s_reinit_in_flight.store(false, std::memory_order_release);
        ui_set_status(a, "REINIT task create failed");
    }
}

bool app_uvc_host_services_init(app_t *a)
{
    if (!a) return false;

    a->events = xEventGroupCreate();
    if (!a->events) {
        ESP_LOGE(TAG, "event group fail");
        return false;
    }
    a->frame_queue = xQueueCreate(CAM_QUEUE_DEPTH, sizeof(uvc_host_frame_t *));
    if (!a->frame_queue) {
        ESP_LOGE(TAG, "queue fail");
        return false;
    }
    a->audio_rec_mutex = xSemaphoreCreateMutex();
    if (!a->audio_rec_mutex) {
        ESP_LOGE(TAG, "audio rec mutex fail");
        return false;
    }

    // 初始化高级控件能力表（探测在 open_stream 后执行）。
    cam_ctrl_init_table(a);

    usb_host_config_t hcfg = {};
    hcfg.skip_phy_setup = false;
    hcfg.intr_flags = ESP_INTR_FLAG_LEVEL1;
    esp_err_t err = usb_host_install(&hcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
        return false;
    }

    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, nullptr,
                            USB_LIB_TASK_PRIO, nullptr, tskNO_AFFINITY);

    uvc_host_driver_config_t ucfg = {};
    ucfg.driver_task_stack_size = 6 * 1024;
    ucfg.driver_task_priority = USB_LIB_TASK_PRIO - 1;
    ucfg.xCoreID = tskNO_AFFINITY;
    ucfg.create_background_task = true;
    ucfg.event_cb = uvc_driver_event_cb;
    ucfg.user_ctx = a;
    err = uvc_host_install(&ucfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_install: %s", esp_err_to_name(err));
        return false;
    }

    uac_host_driver_config_t acfg = {};
    acfg.create_background_task = true;
    acfg.task_priority = USB_LIB_TASK_PRIO - 2;
    acfg.stack_size = 4096;
    acfg.core_id = 1;
    acfg.callback = uac_driver_event_cb;
    acfg.callback_arg = a;
    err = uac_host_install(&acfg);
    if (err == ESP_OK) {
        a->uac_driver_ready = true;
        ESP_LOGI(TAG, "UAC host installed");
    } else {
        a->uac_driver_ready = false;
        ESP_LOGW(TAG, "uac_host_install failed: %s", esp_err_to_name(err));
    }
    return true;
}

static int app_select_factory_simple_mode(app_t *a)
{
    if (!a || a->mode_count == 0) return -1;

    const int mode_idx = factory_simple_mode_index(a);
    if (mode_idx < 0) {
        ESP_LOGW(TAG, "Factory mode: no supported color stream mode");
        return -1;
    }

    a->selected_mode = mode_idx;
    const cam_mode_t *m = &a->modes[mode_idx];
    ESP_LOGI(TAG, "Factory mode selected: index=%d %s %ux%u @%.1f fps",
             mode_idx, uvc_format_name(m->fmt), m->w, m->h, m->fps);
    return mode_idx;
}

static void app_wait_factory_device_removed(app_t *a, const char *status_text)
{
    if (!a) return;
    factory_ui_set_waiting_status(a, status_text ? status_text : "Waiting for USB camera");
    factory_ui_set_mode_info(a);
    while (a->dev_addr != 0 && usb_addr_present(a->dev_addr)) {
        EventBits_t bits = xEventGroupWaitBits(a->events, EV_DEV_LOST,
                                               pdTRUE, pdFALSE, kUiUsbPollTicks);
        if (bits & EV_DEV_LOST) break;
    }
    app_reset_connected_device_state(a);
    factory_ui_set_waiting_status(a, "Waiting for USB camera");
    factory_ui_set_waiting_info(a, "Camera: waiting");
}

static void app_run_factory_loop(app_t *a)
{
    ESP_LOGI(TAG, "Factory simple mode enabled");
    while (true) {
        // --- Phase A: 等待设备并自动选模 ---
        a->state = APP_STATE_WAITING_USB;
        factory_ui_set_waiting_status(a, "Waiting for USB camera");
        factory_ui_set_waiting_info(a, "Camera: waiting");

        xEventGroupWaitBits(a->events, EV_DEV_CONNECTED,
                            pdTRUE, pdFALSE, portMAX_DELAY);
        a->state = APP_STATE_SELECTING;
        factory_ui_set_waiting_status(a, "Camera connected, opening video");
        factory_ui_set_mode_info(a);

        if ((xEventGroupGetBits(a->events) & EV_DEV_LOST) ||
            (a->dev_addr != 0 && !usb_addr_present(a->dev_addr))) {
            ESP_LOGW(TAG, "Factory mode: device lost before auto start");
            app_reset_connected_device_state(a);
            factory_ui_set_waiting_status(a, "Waiting for USB camera");
            factory_ui_set_waiting_info(a, "Camera: waiting");
            xEventGroupClearBits(a->events,
                                 EV_DEV_LOST | EV_USER_CONFIRMED | EV_UAC_CONNECTED | EV_REINIT_REQUESTED);
            continue;
        }

        if (app_select_factory_simple_mode(a) < 0) {
            app_wait_factory_device_removed(a,
                                            "Factory mode: no supported color mode; replug camera");
            xEventGroupClearBits(a->events,
                                 EV_DEV_LOST | EV_USER_CONFIRMED | EV_UAC_CONNECTED | EV_REINIT_REQUESTED);
            continue;
        }

        app_reset_stream_runtime_flags(a);
        factory_ui_set_waiting_status(a, "Starting video");
        factory_ui_set_mode_info(a);

        // --- Phase B: 视频流 ---
        a->state = APP_STATE_STREAMING;
        otool_lvgl_port_stop();

        ESP_LOGI(TAG, "Free SPIRAM before open: %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        if (!alloc_stream_resources(a)) {
            ESP_LOGE(TAG, "Factory mode: failed to alloc stream buffers");
            a->state = APP_STATE_SELECTING;
            otool_lvgl_port_resume();
            app_wait_factory_device_removed(a,
                                            "Factory mode: buffer alloc failed; replug camera");
            xEventGroupClearBits(a->events,
                                 EV_DEV_LOST | EV_USER_CONFIRMED | EV_UAC_CONNECTED | EV_REINIT_REQUESTED);
            continue;
        }

        app_prepare_stream_restart_state(a);

        if (!open_stream(a, a->selected_mode)) {
            ESP_LOGE(TAG, "Factory mode: failed to open stream");
            free_stream_resources(a);
            a->state = APP_STATE_SELECTING;
            otool_lvgl_port_resume();
            app_wait_factory_device_removed(a,
                                            "Factory mode: stream open failed; replug camera");
            xEventGroupClearBits(a->events,
                                 EV_DEV_LOST | EV_USER_CONFIRMED | EV_UAC_CONNECTED | EV_REINIT_REQUESTED);
            continue;
        }

        xEventGroupClearBits(a->events, EV_DEV_LOST | EV_DEV_CONNECTED);

        video_loop(a);

        // --- Phase C: 设备断开后回到等待 ---
        ESP_LOGI(TAG, "Factory mode: stream ended, waiting for next camera");

        app_cleanup_after_stream(a);

        EventBits_t pending_bits = xEventGroupGetBits(a->events);
        bool dev_reconnected = (pending_bits & EV_DEV_CONNECTED) != 0;

        if (!dev_reconnected) {
            app_reset_connected_device_state(a);
        }
        app_reset_stream_runtime_flags(a);
        factory_ui_set_waiting_status(
            a,
            dev_reconnected ? "Camera reconnected, opening video" : "Waiting for USB camera");
        if (dev_reconnected) {
            factory_ui_set_mode_info(a);
        } else {
            factory_ui_set_waiting_info(a, "Camera: waiting");
        }
        xEventGroupClearBits(a->events,
                             dev_reconnected ?
                                 (EV_DEV_LOST | EV_USER_CONFIRMED | EV_UAC_CONNECTED | EV_REINIT_REQUESTED) :
                                 (EV_DEV_LOST | EV_DEV_CONNECTED | EV_USER_CONFIRMED | EV_UAC_CONNECTED | EV_REINIT_REQUESTED));
    }
}

static void app_restore_selection_after_stream_alloc_fail(app_t *a, const char *status_text)
{
    if (!a) return;
    otool_lvgl_port_resume();
    ui_set_status(a, status_text);
    if (otool_lvgl_port_lock(50)) {
        if (a->confirm_btn) lv_obj_clear_state(a->confirm_btn, LV_STATE_DISABLED);
        if (a->check_btn) lv_obj_clear_state(a->check_btn, LV_STATE_DISABLED);
        ui_update_advanced_button(a);
        otool_lvgl_port_unlock();
    }
    a->state = APP_STATE_SELECTING;
}

static void app_run_selection_loop(app_t *a)
{
    while (true) {
        // --- Phase A: 等待设备与用户确认 ---
        a->state = APP_STATE_WAITING_USB;

        xEventGroupWaitBits(a->events, EV_DEV_CONNECTED,
                            pdTRUE, pdFALSE, portMAX_DELAY);
        a->state = APP_STATE_SELECTING;

        ui_set_status(a, "Camera connected, reading CONFIGURATION...");
        bool config_preflight_ok = false;
        if (a->mode_count > 0) {
            config_preflight_ok = probe_stream_configuration(a, a->selected_mode);
        }

        if ((xEventGroupGetBits(a->events) & EV_DEV_LOST) ||
            (a->dev_addr != 0 && !usb_addr_present(a->dev_addr))) {
            ESP_LOGW(TAG, "Device lost during CONFIGURATION preflight");
            app_handle_selecting_device_lost(a, "Camera disconnected, waiting...");
            continue;
        }

        app_refresh_connected_selection_ui(
            a,
            config_preflight_ok ?
                "CONFIGURATION verified from UVC GET_*" :
                "Select mode and press START");

        // 等待用户确认、设备断开、热插拔重连，或稍后检测到 UAC。
        while (true) {
            EventBits_t bits = xEventGroupWaitBits(a->events,
                                                   EV_USER_CONFIRMED | EV_DEV_LOST | EV_DEV_CONNECTED |
                                                       EV_UAC_CONNECTED | EV_REINIT_REQUESTED,
                                                   pdTRUE, pdFALSE, kUiUsbPollTicks);
            if ((bits & (EV_DEV_LOST | EV_DEV_CONNECTED)) == 0 && a->dev_addr != 0 &&
                !usb_addr_present(a->dev_addr) &&
                (xEventGroupGetBits(a->events) & EV_DEV_CONNECTED) == 0) {
                bits |= EV_DEV_LOST;
            }
            if (bits & EV_REINIT_REQUESTED) {
                ESP_LOGI(TAG, "REINIT request observed in selecting state");
            }
            if (bits & EV_UAC_CONNECTED) {
                if (otool_lvgl_port_lock(50)) {
                    ui_update_uac_option(a);
                    otool_lvgl_port_unlock();
                }
            }
            if (bits & EV_DEV_LOST) {
                ESP_LOGW(TAG, "Device lost before confirm");
                app_handle_selecting_device_lost(a, "Camera disconnected, waiting...");
                continue;
            }
            if (bits & EV_DEV_CONNECTED) {
                // 热插拔：摄像头重新上电后会以新地址枚举，这里刷新选择面板。
                ESP_LOGI(TAG, "Hot-plug: device reconnected, verifying CONFIGURATION");
                ui_set_status(a, "Camera reconnected, reading CONFIGURATION...");
                bool hotplug_config_ok = false;
                if (a->mode_count > 0) {
                    hotplug_config_ok = probe_stream_configuration(a, a->selected_mode);
                }
                if ((xEventGroupGetBits(a->events) & EV_DEV_LOST) ||
                    (a->dev_addr != 0 && !usb_addr_present(a->dev_addr))) {
                    ESP_LOGW(TAG, "Device lost during hot-plug CONFIGURATION preflight");
                    app_handle_selecting_device_lost(a, "Camera disconnected, waiting...");
                    continue;
                }
                app_refresh_connected_selection_ui(
                    a,
                    hotplug_config_ok ?
                        "CONFIGURATION verified from UVC GET_*" :
                        "Select mode and press START");
                continue;
            }
            if (bits & EV_USER_CONFIRMED) {
                break;
            }
        }

        // --- Phase B: 视频流 ---
        a->state = APP_STATE_STREAMING;
        a->stream_lvgl_ui = false;
        otool_lvgl_port_stop();

        ESP_LOGI(TAG, "Free SPIRAM before open: %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        if (!alloc_stream_resources(a)) {
            ESP_LOGE(TAG, "Failed to alloc stream buffers");
            app_restore_selection_after_stream_alloc_fail(a, "Stream buffer alloc failed");
            continue;
        }

        app_prepare_stream_restart_state(a);

        if (!open_stream(a, a->selected_mode)) {
            ESP_LOGE(TAG, "Failed to open stream");
            free_stream_resources(a);
            app_restore_selection_after_stream_alloc_fail(a, "Stream open failed, select again");
            continue;
        }

        if (a->uac_selected && !start_uac_stream(a)) {
            ESP_LOGW(TAG, "UAC selected but audio stream was not started");
        }

        xEventGroupClearBits(a->events, EV_DEV_LOST | EV_DEV_CONNECTED);

        video_loop(a);

        // --- Phase C: 设备断开，返回 UI ---
        ESP_LOGI(TAG, "Stream ended, returning to selection UI");

        app_cleanup_after_stream(a);

        EventBits_t pending_bits = xEventGroupGetBits(a->events);
        bool dev_reconnected = (pending_bits & EV_DEV_CONNECTED) != 0;

        if (!dev_reconnected) {
            app_reset_connected_device_state(a);
        }
        app_reset_stream_runtime_flags(a);
        app_refresh_waiting_selection_ui(
            a,
            dev_reconnected ? "Camera reconnected, refreshing..." : "Camera lost. Waiting for USB camera...");
        xEventGroupClearBits(a->events,
                             dev_reconnected ?
                                 (EV_DEV_LOST | EV_USER_CONFIRMED | EV_UAC_CONNECTED | EV_REINIT_REQUESTED) :
                                 (EV_DEV_LOST | EV_DEV_CONNECTED | EV_USER_CONFIRMED | EV_UAC_CONNECTED | EV_REINIT_REQUESTED));
    }
}

void app_run_main_loop(app_t *a)
{
    if (!a) return;
    if (a->factory_simple_mode_active) {
        app_run_factory_loop(a);
    } else {
        app_run_selection_loop(a);
    }
}
