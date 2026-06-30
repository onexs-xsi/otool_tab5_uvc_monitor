/*
 * SPDX-FileCopyrightText: 2026 onexs-xsi
 *
 * SPDX-License-Identifier: MIT
 */

// 应用启动运行时：板卡识别、TAB5/LVGL 初始化与首屏 UI 构建。
#include "app_internal.h"

#include "M5Autodetect.h"
#include "otool_tab5_component.h"
#include "esp_system.h"

const char *TAG = "uvc_cam";
app_t s_app;
portMUX_TYPE s_audio_scope_lock = portMUX_INITIALIZER_UNLOCKED;

// TAB5 板级组件全局实例，供启动流程和 USB5V 电源控制共用。
static m5::tab5::otool_tab5_component s_comp;

esp_err_t app_usb5v_set(bool enable, const char *reason_name)
{
    const char *reason = reason_name ? reason_name : "unknown";
    ESP_LOGI(TAG, "USB5V %s (%s)", enable ? "ON" : "OFF", reason);
    return s_comp.usb5v_enable(enable);
}

void app_reboot_if_camera_lost_mode(app_t *a, const char *reason_name)
{
    if (!a || !a->auto_reboot_on_camera_lost) return;

    const char *reason = reason_name ? reason_name : "camera lost";
    ESP_LOGW(TAG, "Camera lost in auto-reboot startup mode, restarting: %s", reason);
    esp_restart();
}

static bool app_init_board_and_tab5(app_t *a)
{
    if (!a) return false;

    static M5Autodetect autodetect;
    autodetect.begin(M5Autodetect::debug_verbose);
    const m5::autodetect::DeviceInfo *info = autodetect.detect();
    if (!info) {
        ESP_LOGE(TAG, "No device detected");
        return false;
    }
    ESP_LOGI(TAG, "Board: %s id=%d", info->name, info->board_id);

    m5::tab5::M5TAB5_VariantId vid;
    switch (info->board_id) {
    case m5::autodetect::board_M5Tab5_ST7123:
        vid = m5::tab5::M5TAB5_VARIANT_TAB5_LCD_ST7123_TOUCH_ST7123;
        break;
    case m5::autodetect::board_M5Tab5_IlI9881c:
        vid = m5::tab5::M5TAB5_VARIANT_TAB5_LCD_ILI9881_TOUCH_GT911;
        break;
    case m5::autodetect::board_M5Tab5_ST7121:
        vid = m5::tab5::M5TAB5_VARIANT_TAB5_LCD_ST7121_TOUCH_ST7121;
        break;
    default:
        ESP_LOGW(TAG, "Unknown board id=%d, use reference", info->board_id);
        vid = m5::tab5::M5TAB5_VARIANT_REFERENCE;
        break;
    }

    m5::tab5::otool_tab5_component_config_t comp_cfg;
    comp_cfg.variant_id = vid;
    comp_cfg.enable_optional_drivers = true;
    esp_err_t err = s_comp.begin(comp_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tab5 begin: %s", esp_err_to_name(err));
        return false;
    }

    if (TAB5_DIAG_CYCLE_USB5V_ON_BOOT) {
        ESP_LOGW(TAG, "USB5V diagnostic power-cycle enabled");
        err = app_usb5v_set(false, "diagnostic boot cycle");
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "usb5v off: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(700));
    }

    err = app_usb5v_set(true, "startup");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb5v: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "USB 5V ON");
    return true;
}

static bool app_init_lvgl(app_t *a)
{
    if (!a) return false;

    otool_lvgl_port_cfg_t lvgl_cfg = OTOOL_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 16 * 1024;
    lvgl_cfg.task_affinity = 1;
    esp_err_t err = otool_lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init: %s", esp_err_to_name(err));
        return false;
    }

    a->panel = s_comp.lcd_panel();

    const otool_lvgl_disp_cfg_t disp_cfg = {
        .io_handle = nullptr,
        .panel_handle = a->panel,
        .control_handle = nullptr,
        .buffer_size = PANEL_PHYS_W * PANEL_PHYS_H,
        .double_buffer = false,
        .trans_size = 0,
        .hres = PANEL_PHYS_W,
        .vres = PANEL_PHYS_H,
        .monochrome = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 0,
            .buff_spiram = 1,
            .sw_rotate = 1,
            .swap_bytes = 0,
            .full_refresh = 0,
            .direct_mode = 1,
        },
    };
    const otool_lvgl_disp_dsi_cfg_t dsi_cfg = {
        .sw_rotation = LV_DISPLAY_ROTATION_90,
        .flags = { .avoid_tearing = 1, .use_ppa = 1 },
    };
    a->lv_disp = otool_lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!a->lv_disp) {
        ESP_LOGE(TAG, "add_disp_dsi failed");
        return false;
    }

    a->touch_handle = s_comp.touch_panel();
    if (a->touch_handle) {
        const otool_lvgl_touch_cfg_t tcfg = {
            .disp = a->lv_disp,
            .handle = a->touch_handle,
            .scale = { .x = 1.0f, .y = 1.0f },
            .initial_rotation = LV_DISPLAY_ROTATION_90,
        };
        a->lv_touch_indev = otool_lvgl_port_add_touch(&tcfg);
        if (a->lv_touch_indev) {
            otool_lvgl_port_set_touch_rotation(a->lv_touch_indev, LV_DISPLAY_ROTATION_90);
        }
    }

    return true;
}

static void app_build_first_screen(app_t *a, app_startup_mode_t startup_mode)
{
    a->factory_simple_mode_active =
        (startup_mode == APP_STARTUP_MODE_FACTORY_SIMPLE) ||
        (startup_mode == APP_STARTUP_MODE_FACTORY_SIMPLE_AUTO_REBOOT);
    a->auto_reboot_on_camera_lost =
        (startup_mode == APP_STARTUP_MODE_FACTORY_SIMPLE_AUTO_REBOOT);
    if (a->factory_simple_mode_active) {
        factory_ui_build_waiting(a);
    } else {
        ui_build_selection(a);
    }
    a->state = APP_STATE_WAITING_USB;
    ESP_LOGI(TAG, "Init done, waiting for UVC camera...");
}

bool app_bootstrap(app_t *a, app_startup_mode_t startup_mode)
{
    if (!a) return false;
    if (!app_init_board_and_tab5(a)) return false;
    if (!app_init_lvgl(a)) return false;
    if (!app_uvc_host_services_init(a)) return false;
    app_build_first_screen(a, startup_mode);
    return true;
}
