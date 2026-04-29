// UVC Camera Display Demo - M5 TAB5 (ESP32-P4)
//
// 主流程：
//   1. 上电 -> 初始化硬件 -> LVGL 启动
//   2. 等待 UVC 相机 -> 枚举格式
//   3. LVGL 选择 UI 显示 MJPEG/YUY2 模式，并提供可选 UAC 音频开关
//   4. LVGL 停止（otool_lvgl_port_stop）
//   5. 视频流显示：MJPEG -> 硬件 JPEG 解码，YUY2 -> 软件转 RGB565 -> PPA -> draw_bitmap
//      若启用 UAC/ADV，在视频 framebuffer 上直接叠加触摸控件
//      非 720p 分辨率会自动等比缩放到屏幕
//   6. 相机断开 -> 返回 LVGL 选择 UI
//
// 本文件仅保留 app_main，其余功能拆分到独立模块（见 app_internal.h）。

#include <atomic>

#include "app_internal.h"

#include "M5Autodetect.h"
#include "otool_tab5_component.h"

// ---------------------------------------------------------------------------
// 全局状态定义（在 app_internal.h 中以 extern 声明）
// ---------------------------------------------------------------------------
const char *TAG = "uvc_cam";
app_t s_app;
portMUX_TYPE s_audio_scope_lock = portMUX_INITIALIZER_UNLOCKED;

// TAB5 板级组件全局实例，供 REINIT 路径重新控制 USB5V 使用。
static m5::tab5::otool_tab5_component s_comp;
static std::atomic<bool> s_reinit_in_flight{false};

enum class usb5v_power_reason_t {
    startup_enable,
    user_reinit_disable,
    user_reinit_enable,
};

static esp_err_t usb5v_set_allowed(usb5v_power_reason_t reason)
{
    bool enable = true;
    const char *reason_name = "startup";
    switch (reason) {
    case usb5v_power_reason_t::startup_enable:
        enable = true;
        reason_name = "startup";
        break;
    case usb5v_power_reason_t::user_reinit_disable:
        enable = false;
        reason_name = "user REINIT";
        break;
    case usb5v_power_reason_t::user_reinit_enable:
        enable = true;
        reason_name = "user REINIT";
        break;
    }

    ESP_LOGI(TAG, "USB5V %s (%s)", enable ? "ON" : "OFF", reason_name);
    return s_comp.usb5v_enable(enable);
}

static void usb_reinit_task(void *arg)
{
    app_t *aa = (app_t *)arg;
    ESP_LOGI(TAG, "REINIT: powering USB5V OFF");
    esp_err_t err = usb5v_set_allowed(usb5v_power_reason_t::user_reinit_disable);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "REINIT: usb5v OFF returned %s", esp_err_to_name(err));
    }
    if (aa) ui_set_status(aa, "USB power OFF, waiting...");
    vTaskDelay(pdMS_TO_TICKS(800));
    ESP_LOGI(TAG, "REINIT: powering USB5V ON");
    err = usb5v_set_allowed(usb5v_power_reason_t::user_reinit_enable);
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

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------
extern "C" void app_main(void)
{
    // ------------------------------------------------------------------
    // 1. 板卡自检
    // ------------------------------------------------------------------
    static M5Autodetect autodetect;
    autodetect.begin(M5Autodetect::debug_info);
    const m5::autodetect::DeviceInfo *info = autodetect.detect();
    if (!info) { ESP_LOGE(TAG, "No device detected"); return; }
    ESP_LOGI(TAG, "Board: %s id=%d", info->name, info->board_id);

    m5::tab5::M5TAB5_VariantId vid;
    switch (info->board_id) {
    case m5::autodetect::board_M5Tab5_ST7123:
        vid = m5::tab5::M5TAB5_VARIANT_TAB5_LCD_ST7123_TOUCH_ST7123; break;
    case m5::autodetect::board_M5Tab5_IlI9881c:
        vid = m5::tab5::M5TAB5_VARIANT_TAB5_LCD_ILI9881_TOUCH_GT911; break;
    case m5::autodetect::board_M5Tab5_ST7121:
        vid = m5::tab5::M5TAB5_VARIANT_TAB5_LCD_ST7121_TOUCH_ST7121; break;
    default:
        ESP_LOGW(TAG, "Unknown board id=%d, use reference", info->board_id);
        vid = m5::tab5::M5TAB5_VARIANT_REFERENCE; break;
    }

    // ------------------------------------------------------------------
    // 2. TAB5 组件初始化
    // ------------------------------------------------------------------
    auto &comp = s_comp;
    m5::tab5::otool_tab5_component_config_t comp_cfg;
    comp_cfg.variant_id              = vid;
    comp_cfg.enable_optional_drivers = true;
    esp_err_t err = comp.begin(comp_cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "tab5 begin: %s", esp_err_to_name(err)); return; }

    // ------------------------------------------------------------------
    // 3. USB 5V 输出
    // ------------------------------------------------------------------
    err = usb5v_set_allowed(usb5v_power_reason_t::startup_enable);
    if (err != ESP_OK) { ESP_LOGE(TAG, "usb5v: %s", esp_err_to_name(err)); return; }
    ESP_LOGI(TAG, "USB 5V ON");

    // ------------------------------------------------------------------
    // 4. LVGL port 初始化
    // ------------------------------------------------------------------
    otool_lvgl_port_cfg_t lvgl_cfg = OTOOL_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack    = 16 * 1024;
    lvgl_cfg.task_affinity = 1;
    err = otool_lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "lvgl_port_init: %s", esp_err_to_name(err)); return; }

    s_app.panel = comp.lcd_panel();

    const otool_lvgl_disp_cfg_t disp_cfg = {
        .io_handle      = nullptr,
        .panel_handle   = s_app.panel,
        .control_handle = nullptr,
        .buffer_size    = PANEL_PHYS_W * PANEL_PHYS_H,
        .double_buffer  = false,
        .trans_size     = 0,
        .hres           = PANEL_PHYS_W,
        .vres           = PANEL_PHYS_H,
        .monochrome     = false,
        .rotation       = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .color_format   = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma    = 0, .buff_spiram = 1, .sw_rotate = 1,
            .swap_bytes  = 0, .full_refresh = 0, .direct_mode = 1,
        },
    };
    const otool_lvgl_disp_dsi_cfg_t dsi_cfg = {
        .sw_rotation = LV_DISPLAY_ROTATION_90,
        .flags = { .avoid_tearing = 1, .use_ppa = 1 },
    };
    s_app.lv_disp = otool_lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!s_app.lv_disp) { ESP_LOGE(TAG, "add_disp_dsi failed"); return; }

    s_app.touch_handle = comp.touch_panel();
    if (s_app.touch_handle) {
        const otool_lvgl_touch_cfg_t tcfg = {
            .disp             = s_app.lv_disp,
            .handle           = s_app.touch_handle,
            .scale            = { .x = 1.0f, .y = 1.0f },
            .initial_rotation = LV_DISPLAY_ROTATION_90,
        };
        s_app.lv_touch_indev = otool_lvgl_port_add_touch(&tcfg);
        if (s_app.lv_touch_indev)
            otool_lvgl_port_set_touch_rotation(s_app.lv_touch_indev, LV_DISPLAY_ROTATION_90);
    }

    // ------------------------------------------------------------------
    // 5. USB Host + UVC + UAC 驱动
    // ------------------------------------------------------------------
    s_app.events = xEventGroupCreate();
    if (!s_app.events) { ESP_LOGE(TAG, "event group fail"); return; }
    s_app.frame_queue = xQueueCreate(CAM_QUEUE_DEPTH, sizeof(uvc_host_frame_t *));
    if (!s_app.frame_queue) { ESP_LOGE(TAG, "queue fail"); return; }
    s_app.audio_rec_mutex = xSemaphoreCreateMutex();
    if (!s_app.audio_rec_mutex) { ESP_LOGE(TAG, "audio rec mutex fail"); return; }

    // 初始化高级控件能力表（探测在 open_stream 后执行）
    cam_ctrl_init_table(&s_app);

    {
        usb_host_config_t hcfg = {};
        hcfg.skip_phy_setup = false;
        hcfg.intr_flags     = ESP_INTR_FLAG_LEVEL1;
        err = usb_host_install(&hcfg);
    }
    if (err != ESP_OK) { ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err)); return; }

    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, nullptr,
                            USB_LIB_TASK_PRIO, nullptr, tskNO_AFFINITY);

    {
        uvc_host_driver_config_t ucfg = {};
        ucfg.driver_task_stack_size = 6 * 1024;
        ucfg.driver_task_priority   = USB_LIB_TASK_PRIO - 1;
        ucfg.xCoreID               = tskNO_AFFINITY;
        ucfg.create_background_task = true;
        ucfg.event_cb               = uvc_driver_event_cb;
        ucfg.user_ctx               = &s_app;
        err = uvc_host_install(&ucfg);
    }
    if (err != ESP_OK) { ESP_LOGE(TAG, "uvc_host_install: %s", esp_err_to_name(err)); return; }

    {
        uac_host_driver_config_t acfg = {};
        acfg.create_background_task = true;
        acfg.task_priority          = USB_LIB_TASK_PRIO - 2;
        acfg.stack_size             = 4096;
        acfg.core_id                = 1;
        acfg.callback               = uac_driver_event_cb;
        acfg.callback_arg           = &s_app;
        err = uac_host_install(&acfg);
    }
    if (err == ESP_OK) {
        s_app.uac_driver_ready = true;
        ESP_LOGI(TAG, "UAC host installed");
    } else {
        s_app.uac_driver_ready = false;
        ESP_LOGW(TAG, "uac_host_install failed: %s", esp_err_to_name(err));
    }

    // ------------------------------------------------------------------
    // 6. 构建选择 UI（首屏始终显示）
    // ------------------------------------------------------------------
    ui_build_selection(&s_app);
    s_app.state = APP_STATE_WAITING_USB;
    ESP_LOGI(TAG, "Init done, waiting for UVC camera...");

    // ------------------------------------------------------------------
    // 主循环：app_main 任务内的状态机
    // ------------------------------------------------------------------
    while (true) {
        // --- Phase A: 等待设备与用户确认 ---
        s_app.state = APP_STATE_WAITING_USB;

        xEventGroupWaitBits(s_app.events, EV_DEV_CONNECTED,
                            pdTRUE, pdFALSE, portMAX_DELAY);
        s_app.state = APP_STATE_SELECTING;

        if (otool_lvgl_port_lock(0)) {
            ui_populate_mode_list(&s_app);
            if (s_app.status_lbl)
                lv_label_set_text(s_app.status_lbl,
                    true ?
                    "Select mode and press START" :
                    "Only lower-resolution modes are enabled");
            ui_update_uac_option(&s_app);
            if (s_app.confirm_btn) {
                if (true)
                    lv_obj_clear_state(s_app.confirm_btn, LV_STATE_DISABLED);
                else
                    lv_obj_add_state(s_app.confirm_btn, LV_STATE_DISABLED);
            }
            if (s_app.check_btn) {
                if (s_app.mode_count > 0 && !s_app.check_running)
                    lv_obj_clear_state(s_app.check_btn, LV_STATE_DISABLED);
                else
                    lv_obj_add_state(s_app.check_btn, LV_STATE_DISABLED);
            }
            ui_update_info_panel(&s_app);
            ui_update_advanced_button(&s_app);
            otool_lvgl_port_unlock();
        }

        // 等待用户确认、设备断开、热插拔重连，或稍后检测到 UAC。
        while (true) {
            EventBits_t bits = xEventGroupWaitBits(s_app.events,
                                                   EV_USER_CONFIRMED | EV_DEV_LOST | EV_DEV_CONNECTED |
                                                       EV_UAC_CONNECTED | EV_REINIT_REQUESTED,
                                                   pdTRUE, pdFALSE, portMAX_DELAY);
            if (bits & EV_REINIT_REQUESTED) {
                ESP_LOGI(TAG, "REINIT request observed in selecting state");
            }
            if (bits & EV_UAC_CONNECTED) {
                if (otool_lvgl_port_lock(50)) {
                    ui_update_uac_option(&s_app);
                    otool_lvgl_port_unlock();
                }
            }
            if (bits & EV_DEV_LOST) {
                ESP_LOGW(TAG, "Device lost before confirm");
                ui_set_status(&s_app, "Camera disconnected, waiting...");
                if (otool_lvgl_port_lock(50)) {
                    if (s_app.confirm_btn)
                        lv_obj_add_state(s_app.confirm_btn, LV_STATE_DISABLED);
                    if (s_app.check_btn)
                        lv_obj_add_state(s_app.check_btn, LV_STATE_DISABLED);
                    if (s_app.adv_btn)
                        lv_obj_add_state(s_app.adv_btn, LV_STATE_DISABLED);
                    if (s_app.adv_switch)
                        lv_obj_add_state(s_app.adv_switch, LV_STATE_DISABLED);
                    ui_clear_mode_list(&s_app);
                    ui_update_info_panel(&s_app);
                    ui_update_advanced_button(&s_app);
                    otool_lvgl_port_unlock();
                }
                s_app.mode_count = 0;
                s_app.selected_mode = 0;
                s_app.adv_touch_down = false;
                continue;
            }
            if (bits & EV_DEV_CONNECTED) {
                // 热插拔：摄像头重新上电后会以新地址枚举，这里刷新选择面板。
                ESP_LOGI(TAG, "Hot-plug: device reconnected, refreshing mode list");
                if (otool_lvgl_port_lock(0)) {
                    ui_populate_mode_list(&s_app);
                    if (s_app.status_lbl)
                        lv_label_set_text(s_app.status_lbl, "Select mode and press START");
                    ui_update_uac_option(&s_app);
                    if (s_app.confirm_btn) {
                        if (s_app.mode_count > 0)
                            lv_obj_clear_state(s_app.confirm_btn, LV_STATE_DISABLED);
                        else
                            lv_obj_add_state(s_app.confirm_btn, LV_STATE_DISABLED);
                    }
                    if (s_app.check_btn) {
                        if (s_app.mode_count > 0 && !s_app.check_running)
                            lv_obj_clear_state(s_app.check_btn, LV_STATE_DISABLED);
                        else
                            lv_obj_add_state(s_app.check_btn, LV_STATE_DISABLED);
                    }
                    ui_update_info_panel(&s_app);
                    ui_update_advanced_button(&s_app);
                    otool_lvgl_port_unlock();
                }
                continue;
            }
            if (bits & EV_USER_CONFIRMED) {
                break;
            }
        }

        // --- Phase B: 视频流 ---
        s_app.state = APP_STATE_STREAMING;
        s_app.stream_lvgl_ui = false;
        otool_lvgl_port_stop();

        ESP_LOGI(TAG, "Free SPIRAM before open: %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        if (!alloc_stream_resources(&s_app)) {
            ESP_LOGE(TAG, "Failed to alloc stream buffers");
            otool_lvgl_port_resume();
            ui_set_status(&s_app, "Stream buffer alloc failed");
            if (otool_lvgl_port_lock(50)) {
                if (s_app.confirm_btn)
                    lv_obj_clear_state(s_app.confirm_btn, LV_STATE_DISABLED);
                if (s_app.check_btn)
                    lv_obj_clear_state(s_app.check_btn, LV_STATE_DISABLED);
                ui_update_advanced_button(&s_app);
                otool_lvgl_port_unlock();
            }
            s_app.state = APP_STATE_SELECTING;
            continue;
        }

        panel_clear(&s_app);

        if (!open_stream(&s_app, s_app.selected_mode)) {
            ESP_LOGE(TAG, "Failed to open stream");
            free_stream_resources(&s_app);
            otool_lvgl_port_resume();
            ui_set_status(&s_app, "Stream open failed, select again");
            if (otool_lvgl_port_lock(50)) {
                if (s_app.confirm_btn)
                    lv_obj_clear_state(s_app.confirm_btn, LV_STATE_DISABLED);
                if (s_app.check_btn)
                    lv_obj_clear_state(s_app.check_btn, LV_STATE_DISABLED);
                ui_update_advanced_button(&s_app);
                otool_lvgl_port_unlock();
            }
            s_app.state = APP_STATE_SELECTING;
            continue;
        }

        if (s_app.uac_selected && !start_uac_stream(&s_app)) {
            ESP_LOGW(TAG, "UAC selected but audio stream was not started");
        }

        xEventGroupClearBits(s_app.events, EV_DEV_LOST | EV_DEV_CONNECTED);

        video_loop(&s_app);

        // --- Phase C: 设备断开，返回 UI ---
        ESP_LOGI(TAG, "Stream ended, returning to selection UI");

        stop_uac_stream(&s_app);

        if (s_app.stream_hdl) {
            uvc_host_stream_stop(s_app.stream_hdl);
            drain_queue(&s_app);
            uvc_host_stream_close(s_app.stream_hdl);
            s_app.stream_hdl = nullptr;
        } else {
            drain_queue(&s_app);
        }

        free_stream_resources(&s_app);

        ui_destroy_stream_overlay(&s_app);
        otool_lvgl_port_resume();
        s_app.state = APP_STATE_WAITING_USB;

        if (otool_lvgl_port_lock(50)) {
            ui_clear_mode_list(&s_app);
            if (s_app.status_lbl)
                lv_label_set_text(s_app.status_lbl, "Camera lost. Waiting for USB camera...");
            ui_update_uac_option(&s_app);
            if (s_app.confirm_btn)
                lv_obj_add_state(s_app.confirm_btn, LV_STATE_DISABLED);
            if (s_app.check_btn)
                lv_obj_add_state(s_app.check_btn, LV_STATE_DISABLED);
            ui_update_info_panel(&s_app);
            ui_update_advanced_button(&s_app);
            otool_lvgl_port_unlock();
        }
        s_app.mode_count = 0;
        s_app.selected_mode = 0;
        s_app.uac_selected = false;
        s_app.adv_live_enabled = false;
        s_app.adv_touch_down = false;
        s_app.stream_lvgl_ui = false;
        xEventGroupClearBits(s_app.events, EV_DEV_LOST | EV_DEV_CONNECTED | EV_USER_CONFIRMED | EV_UAC_CONNECTED | EV_REINIT_REQUESTED);
    }
    // unreachable
}
