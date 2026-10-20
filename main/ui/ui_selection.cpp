/*
 * SPDX-FileCopyrightText: 2026 onexs-xsi
 *
 * SPDX-License-Identifier: MIT
 */

// 自动从原 main.cpp 拆分而来。仅供 main 组件内部使用。
#include "app_internal.h"
#include "display_viewport.h"

#ifndef TAB5_APP_VERSION_BUILD
#define TAB5_APP_VERSION_BUILD "vdev_unknown"
#endif

// 文件内静态前向声明（事件回调互相引用）
static void check_close_cb(lv_event_t *e);
static void btn_check_cb(lv_event_t *e);
static void btn_reinit_cb(lv_event_t *e);
static void btn_advanced_cb(lv_event_t *e);
static void adv_switch_cb(lv_event_t *e);
static void adv_option_click_cb(lv_event_t *e);
static void adv_close_cb(lv_event_t *e);
static void adv_overlay_bg_cb(lv_event_t *e);
static void adv_widget_changed_cb(lv_event_t *e);
static void adv_step_click_cb(lv_event_t *e);
static void confirm_not_recommended_open_cb(lv_event_t *e);
static void confirm_not_recommended_cancel_cb(lv_event_t *e);
static bool adv_any_supported(const app_t *a);

// ===================================================================
// LVGL selection UI
// ===================================================================
static void ui_set_panel_style(lv_obj_t *obj, lv_color_t bg, uint8_t radius)
{
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_make(0xD8, 0xE2, 0xF0), 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_shadow_width(obj, 18, 0);
    lv_obj_set_style_shadow_ofs_y(obj, 6, 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_10, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_make(0x64, 0x74, 0x8B), 0);
    lv_obj_set_scrollable(obj, false);
}

static lv_obj_t *ui_create_info_label(lv_obj_t *parent, const char *text, lv_color_t color, const lv_font_t *font)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_set_style_text_color(lbl, color, 0);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    return lbl;
}

static bool ui_pending_adv_live_enabled(const app_t *a)
{
    return a && adv_any_supported(a) && a->adv_switch &&
           lv_obj_has_state(a->adv_switch, LV_STATE_CHECKED);
}

static display_viewport_t ui_display_viewport_for_mode(const app_t *a, const cam_mode_t *m)
{
    const display_viewport_config_t viewport_config = {
        PANEL_PHYS_W,
        PANEL_PHYS_H,
        ADV_PHY_PY_RESERVE,
        ui_pending_adv_live_enabled(a),
    };
    return display_viewport_calc(m ? m->w : 0, m ? m->h : 0, &viewport_config);
}

static void ui_format_display_viewport_note(const app_t *a, const cam_mode_t *m,
                                            char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    const display_viewport_t viewport = ui_display_viewport_for_mode(a, m);
    display_viewport_format_summary(&viewport, out, out_size);
    if (viewport.reserved_top_h > 0) {
        const size_t used = strlen(out);
        if (used + 1 < out_size) {
            snprintf(out + used, out_size - used,
                     " ADV reserves top %u px.",
                     (unsigned)viewport.reserved_top_h);
        }
    }
}

static void ui_reset_touch_cache(app_t *a)
{
    if (a && a->lv_touch_indev) {
        otool_lvgl_port_touch_reset(a->lv_touch_indev);
    }
}

static bool ui_mode_start_allowed(const app_t *a, int idx)
{
    if (!a || idx < 0 || idx >= (int)a->mode_count) return false;
    const cam_mode_t *m = &a->modes[idx];
    return mode_is_selectable(m) || mode_start_requires_confirmation(m);
}

static void ui_clear_not_recommended_confirm(app_t *a)
{
    if (!a) return;
    if (a->not_recommended_confirm_overlay) {
        ui_reset_touch_cache(a);
        lv_obj_delete(a->not_recommended_confirm_overlay);
    }
    a->not_recommended_confirm_overlay = nullptr;
    a->not_recommended_confirm_mode = -1;
}

static void ui_set_selected_mode_visual(app_t *a, int idx)
{
    if (!a || !a->list_obj) return;
    uint32_t cnt = lv_obj_get_child_count(a->list_obj);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *item = lv_obj_get_child(a->list_obj, i);
        ui_style_mode_item(item, (int)i == idx);
    }
}

static void ui_commit_selected_mode(app_t *a)
{
    if (!a || a->selected_mode < 0 || a->selected_mode >= (int)a->mode_count) return;
    a->uac_selected = a->uac_rx_available && a->uac_switch &&
                      lv_obj_has_state(a->uac_switch, LV_STATE_CHECKED);
    a->adv_live_enabled = adv_any_supported(a) && a->adv_switch &&
                          lv_obj_has_state(a->adv_switch, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "User confirmed mode %d", a->selected_mode);
    xEventGroupSetBits(a->events, EV_USER_CONFIRMED);
}

static bool adv_any_supported(const app_t *a)
{
    if (!a || !a->adv_caps_probed) return false;
    for (int i = 0; i < (int)CAM_ADV_CTRL_COUNT; i++) {
        if (a->adv_caps[i].supported) return true;
    }
    return false;
}

static bool adv_any_candidate(const app_t *a)
{
    if (!a) return false;
    for (int i = 0; i < (int)CAM_ADV_CTRL_COUNT; i++) {
        if (a->adv_caps[i].supported) return true;
    }
    return false;
}

void ui_update_info_panel(app_t *a)
{
    if (!a) return;

    char buf[160];
    if (a->info_mode_lbl) {
        if (a->selected_mode >= 0 && a->selected_mode < (int)a->mode_count) {
            const cam_mode_t *m = &a->modes[a->selected_mode];
            snprintf(buf, sizeof(buf), "Selected\n%s %ux%u\n%.1f fps", uvc_format_name(m->fmt), m->w, m->h, m->fps);
        } else {
            snprintf(buf, sizeof(buf), "Selected\nNo mode");
        }
        lv_label_set_text(a->info_mode_lbl, buf);
    }

    if (a->info_device_lbl) {
        snprintf(buf, sizeof(buf), "Device\naddr %u / stream %u\n%u modes", a->dev_addr, a->stream_idx, (unsigned)a->mode_count);
        lv_label_set_text(a->info_device_lbl, buf);
    }

    if (a->info_formats_lbl) {
        unsigned mjpeg = 0, mjpeg_sw = 0, yuy2 = 0, nv12 = 0, uyvy = 0, gray8 = 0, rgbp = 0, other = 0;
        for (size_t i = 0; i < a->mode_count; i++) {
            switch (a->modes[i].fmt) {
            case UVC_VS_FORMAT_MJPEG: mjpeg++; break;
            case UVC_VS_FORMAT_MJPEG_SW: mjpeg_sw++; break;
            case UVC_VS_FORMAT_YUY2:  yuy2++; break;
            case UVC_VS_FORMAT_NV12:  nv12++; break;
            case UVC_VS_FORMAT_UYVY:  uyvy++; break;
            case UVC_VS_FORMAT_GRAY8: gray8++; break;
            case UVC_VS_FORMAT_RGBP:  rgbp++; break;
            default:                  other++; break;
            }
        }
        snprintf(buf, sizeof(buf), "Formats\nMJPEG(HW) %u  MJPEG(SW) %u\nYUY2 %u  NV12 %u  UYVY %u\nGRAY8 %u  RGBP %u  Other %u",
                 mjpeg, mjpeg_sw, yuy2, nv12, uyvy, gray8, rgbp, other);
        lv_label_set_text(a->info_formats_lbl, buf);
    }

    if (a->info_audio_lbl) {
        const char *audio = "no UAC";
        if (a->uac_rx_available && a->uac_tx_available) audio = "mic+spk";
        else if (a->uac_rx_available) audio = "mic";
        else if (a->uac_tx_available) audio = "spk";
        snprintf(buf, sizeof(buf), "Audio: %s / %s", audio, a->uac_selected ? "on" : "off");
        lv_label_set_text(a->info_audio_lbl, buf);
    }
}

static void ui_check_set_text(lv_obj_t *lbl, const char *text, lv_color_t color)
{
    if (!lbl) return;
    if (otool_lvgl_port_lock(100)) {
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, color, 0);
        otool_lvgl_port_unlock();
    }
}

static void ui_check_modal_create_locked(app_t *a, lv_obj_t **row_labels, size_t row_label_count)
{
    if (a->check_overlay) {
        ui_reset_touch_cache(a);
        lv_obj_delete(a->check_overlay);
        a->check_overlay = nullptr;
        a->check_list = nullptr;
        a->check_status_lbl = nullptr;
        a->check_close_btn = nullptr;
    }

    memset(row_labels, 0, sizeof(lv_obj_t *) * row_label_count);

    lv_obj_t *scr = lv_scr_act();
    a->check_overlay = lv_obj_create(scr);
    lv_obj_set_size(a->check_overlay, lv_pct(100), lv_pct(100));
    lv_obj_align(a->check_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(a->check_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(a->check_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(a->check_overlay, 0, 0);
    lv_obj_set_style_radius(a->check_overlay, 0, 0);
    lv_obj_set_style_pad_all(a->check_overlay, 0, 0);
    lv_obj_set_scrollable(a->check_overlay, false);
    lv_obj_set_clickable(a->check_overlay, true);

    lv_obj_t *panel = lv_obj_create(a->check_overlay);
    lv_obj_set_size(panel, 860, 560);
    lv_obj_center(panel);
    ui_set_panel_style(panel, lv_color_make(0xFF, 0xFF, 0xFF), 18);
    lv_obj_set_style_pad_all(panel, 22, 0);
    lv_obj_set_layout(panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *title = ui_create_info_label(panel, "Camera capability check", lv_color_make(0x0F, 0x17, 0x2A), &lv_font_montserrat_24);
    lv_obj_set_height(title, 34);

    lv_obj_t *desc = ui_create_info_label(panel, "Each listed UVC mode is requested and sampled 5 times.", lv_color_make(0x64, 0x74, 0x8B), nullptr);
    lv_obj_set_height(desc, 26);

    a->check_status_lbl = ui_create_info_label(panel, "Preparing...", lv_color_make(0x25, 0x63, 0xEB), nullptr);
    lv_obj_set_height(a->check_status_lbl, 26);

    a->check_list = lv_obj_create(panel);
    lv_obj_set_width(a->check_list, lv_pct(100));
    lv_obj_set_height(a->check_list, 350);
    lv_obj_set_style_bg_color(a->check_list, lv_color_make(0xF8, 0xFA, 0xFC), 0);
    lv_obj_set_style_bg_opa(a->check_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(a->check_list, 1, 0);
    lv_obj_set_style_border_color(a->check_list, lv_color_make(0xE2, 0xE8, 0xF0), 0);
    lv_obj_set_style_radius(a->check_list, 12, 0);
    lv_obj_set_style_pad_all(a->check_list, 10, 0);
    lv_obj_set_style_pad_row(a->check_list, 6, 0);
    lv_obj_set_scrollbar_mode(a->check_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(a->check_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(a->check_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->check_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < (int)a->mode_count && i < (int)row_label_count; i++) {
        const cam_mode_t *m = &a->modes[i];
        char row_text[128];
        snprintf(row_text, sizeof(row_text), "WAIT  [%02d] %s %ux%u @%.1f", i, uvc_format_name(m->fmt), m->w, m->h, m->fps);
        lv_obj_t *row = lv_obj_create(a->check_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 42);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_make(0xE2, 0xE8, 0xF0), 0);
        lv_obj_set_style_radius(row, 9, 0);
        lv_obj_set_style_pad_left(row, 12, 0);
        lv_obj_set_scrollable(row, false);
        lv_obj_set_clickable(row, false);
        row_labels[i] = lv_label_create(row);
        lv_label_set_text(row_labels[i], row_text);
        lv_obj_set_style_text_color(row_labels[i], lv_color_make(0x64, 0x74, 0x8B), 0);
        lv_obj_align(row_labels[i], LV_ALIGN_LEFT_MID, 0, 0);
    }

    a->check_close_btn = lv_button_create(panel);
    lv_obj_set_size(a->check_close_btn, 128, 44);
    lv_obj_set_style_radius(a->check_close_btn, 12, 0);
    lv_obj_set_style_bg_color(a->check_close_btn, lv_color_make(0x25, 0x63, 0xEB), 0);
    lv_obj_set_style_bg_color(a->check_close_btn, lv_color_make(0xBF, 0xDB, 0xFE), LV_STATE_DISABLED);
    lv_obj_add_state(a->check_close_btn, LV_STATE_DISABLED);
    lv_obj_add_event_cb(a->check_close_btn, check_close_cb, LV_EVENT_CLICKED, a);
    lv_obj_t *close_lbl = lv_label_create(a->check_close_btn);
    lv_label_set_text(close_lbl, "RUNNING");
    lv_obj_set_style_text_color(close_lbl, lv_color_white(), 0);
    lv_obj_center(close_lbl);

    lv_obj_move_foreground(a->check_overlay);
}

static void close_check_stream(app_t *a)
{
    if (a->stream_hdl) {
        uvc_host_stream_stop(a->stream_hdl);
        drain_queue(a);
        uvc_host_stream_close(a->stream_hdl);
        a->stream_hdl = nullptr;
    } else {
        drain_queue(a);
    }
    a->stream_active = false;
}

static void check_task_entry(void *arg)
{
    app_t *a = (app_t *)arg;
    lv_obj_t *row_labels[CAM_MAX_CANDIDATES] = {};

    if (otool_lvgl_port_lock(100)) {
        ui_check_modal_create_locked(a, row_labels, CAM_MAX_CANDIDATES);
        otool_lvgl_port_unlock();
    }

    int passed = 0;
    for (int i = 0; i < (int)a->mode_count && a->state == APP_STATE_SELECTING; i++) {
        const cam_mode_t *m = &a->modes[i];
        char text[160];
        const char *unavailable = mode_unavailable_reason(m);
        if (unavailable) {
            snprintf(text, sizeof(text), "SKIP  [%02d] %s %ux%u @%.1f  (%s)",
                     i, uvc_format_name(m->fmt), m->w, m->h, m->fps, unavailable);
            ui_check_set_text(row_labels[i], text, lv_color_make(0x94, 0xA3, 0xB8));
            vTaskDelay(pdMS_TO_TICKS(90));
            continue;
        }
        snprintf(text, sizeof(text), "RUN   [%02d] %s %ux%u @%.1f  0/%d", i, uvc_format_name(m->fmt), m->w, m->h, m->fps, CAM_CHECK_ATTEMPTS);
        ui_check_set_text(row_labels[i], text, lv_color_make(0x25, 0x63, 0xEB));
        ui_check_set_text(a->check_status_lbl, "Opening UVC mode...", lv_color_make(0x25, 0x63, 0xEB));

        drain_queue(a);
        bool opened = open_stream(a, i);
        int frames = 0;
        if (opened) {
            for (int attempt = 0; attempt < CAM_CHECK_ATTEMPTS; attempt++) {
                uvc_host_frame_t *frame = nullptr;
                if (xQueueReceive(a->frame_queue, &frame, pdMS_TO_TICKS(CAM_CHECK_FRAME_TIMEOUT_MS)) == pdPASS && frame && frame->data_len > 0) {
                    frames++;
                    uvc_host_stream_hdl_t hdl = a->stream_hdl;
                    if (hdl) uvc_host_frame_return(hdl, frame);
                } else if (frame && a->stream_hdl) {
                    uvc_host_frame_return(a->stream_hdl, frame);
                }
                snprintf(text, sizeof(text), "RUN   [%02d] %s %ux%u @%.1f  %d/%d", i, uvc_format_name(m->fmt), m->w, m->h, m->fps, frames, CAM_CHECK_ATTEMPTS);
                ui_check_set_text(row_labels[i], text, lv_color_make(0x25, 0x63, 0xEB));
            }
            close_check_stream(a);
        }

        bool ok = opened && frames > 0;
        if (ok) passed++;
        snprintf(text, sizeof(text), "%s  [%02d] %s %ux%u @%.1f  frames=%d/%d",
                 ok ? "OK   " : "FAIL ", i, uvc_format_name(m->fmt), m->w, m->h, m->fps, frames, CAM_CHECK_ATTEMPTS);
        ui_check_set_text(row_labels[i], text, ok ? lv_color_make(0x16, 0xA3, 0x4A) : lv_color_make(0xDC, 0x26, 0x26));
        vTaskDelay(pdMS_TO_TICKS(180));
    }

    char done_text[96];
    snprintf(done_text, sizeof(done_text), "Check done: %d/%u modes delivered frames", passed, (unsigned)a->mode_count);
    ui_check_set_text(a->check_status_lbl, done_text, lv_color_make(0x16, 0xA3, 0x4A));

    if (otool_lvgl_port_lock(100)) {
        a->check_running = false;
        if (a->confirm_btn && ui_mode_start_allowed(a, a->selected_mode)) {
            lv_obj_clear_state(a->confirm_btn, LV_STATE_DISABLED);
        }
        if (a->check_btn && a->mode_count > 0) lv_obj_clear_state(a->check_btn, LV_STATE_DISABLED);
        if (a->check_close_btn) {
            lv_obj_clear_state(a->check_close_btn, LV_STATE_DISABLED);
            lv_obj_t *lbl = lv_obj_get_child(a->check_close_btn, 0);
            if (lbl) lv_label_set_text(lbl, "DONE");
        }
        ui_update_info_panel(a);
        ui_update_advanced_button(a);
        otool_lvgl_port_unlock();
    } else {
        a->check_running = false;
    }

    a->check_task = nullptr;
    vTaskDelete(nullptr);
}

static void uac_switch_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED || !a || !a->uac_switch) return;
    if (!a->uac_rx_available) {
        a->uac_selected = false;
        lv_obj_clear_state(a->uac_switch, LV_STATE_CHECKED);
        return;
    }
    a->uac_selected = lv_obj_has_state(a->uac_switch, LV_STATE_CHECKED);
    ui_update_info_panel(a);
}

static void uac_option_click_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !a || !a->uac_switch) return;
    if (!a->uac_rx_available) return;
    if (lv_obj_has_state(a->uac_switch, LV_STATE_CHECKED)) {
        lv_obj_clear_state(a->uac_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_add_state(a->uac_switch, LV_STATE_CHECKED);
    }
    a->uac_selected = lv_obj_has_state(a->uac_switch, LV_STATE_CHECKED);
    ui_update_info_panel(a);
}

void ui_update_uac_option(app_t *a)
{
    if (!a->uac_switch || !a->uac_lbl) return;

    if (a->uac_rx_available) {
        lv_obj_clear_state(a->uac_switch, LV_STATE_DISABLED);
        if (a->uac_tx_available) {
            lv_label_set_text(a->uac_lbl, "Audio: UAC mic + speaker");
        } else {
            lv_label_set_text(a->uac_lbl, "Audio: UAC mic only");
        }
    } else {
        a->uac_selected = false;
        lv_obj_clear_state(a->uac_switch, LV_STATE_CHECKED);
        lv_obj_add_state(a->uac_switch, LV_STATE_DISABLED);
        lv_label_set_text(a->uac_lbl, "Audio: no UAC");
    }
    ui_update_info_panel(a);
}

static void show_not_recommended_confirm_dialog(app_t *a, int idx)
{
    if (!a || idx < 0 || idx >= (int)a->mode_count) return;
    const cam_mode_t *m = &a->modes[idx];
    if (!mode_start_requires_confirmation(m)) return;

    ui_clear_not_recommended_confirm(a);
    a->not_recommended_confirm_mode = idx;

    lv_obj_t *scr = lv_scr_act();
    a->not_recommended_confirm_overlay = lv_obj_create(scr);
    lv_obj_set_size(a->not_recommended_confirm_overlay, lv_pct(100), lv_pct(100));
    lv_obj_align(a->not_recommended_confirm_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(a->not_recommended_confirm_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(a->not_recommended_confirm_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(a->not_recommended_confirm_overlay, 0, 0);
    lv_obj_set_style_radius(a->not_recommended_confirm_overlay, 0, 0);
    lv_obj_set_style_pad_all(a->not_recommended_confirm_overlay, 0, 0);
    lv_obj_set_scrollable(a->not_recommended_confirm_overlay, false);
    lv_obj_set_clickable(a->not_recommended_confirm_overlay, true);

    lv_obj_t *panel = lv_obj_create(a->not_recommended_confirm_overlay);
    lv_obj_set_size(panel, 720, 370);
    lv_obj_center(panel);
    ui_set_panel_style(panel, lv_color_white(), 16);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_set_scrollable(panel, false);

    lv_obj_t *title = ui_create_info_label(panel, "Open video stream?",
                                           lv_color_make(0x0F, 0x17, 0x2A),
                                           &lv_font_montserrat_24);
    lv_obj_set_height(title, 36);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    char display_note[224];
    ui_format_display_viewport_note(a, m, display_note, sizeof(display_note));

    char msg[512];
    snprintf(msg, sizeof(msg),
             "%s %ux%u @%.1f fps is not recommended.\n\n"
             "%s\n\n"
             "Performance may be poor.",
             uvc_format_name(m->fmt), m->w, m->h, m->fps, display_note);
    lv_obj_t *body = ui_create_info_label(panel, msg, lv_color_make(0x47, 0x55, 0x69),
                                          &lv_font_montserrat_16);
    lv_obj_set_width(body, 672);
    lv_obj_set_height(body, 206);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 56);

    lv_obj_t *cancel = lv_button_create(panel);
    lv_obj_set_size(cancel, 150, 48);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, -172, 0);
    lv_obj_set_style_radius(cancel, 12, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_make(0xE2, 0xE8, 0xF0), 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_add_event_cb(cancel, confirm_not_recommended_cancel_cb, LV_EVENT_CLICKED, a);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "CANCEL");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_make(0x0F, 0x17, 0x2A), 0);
    lv_obj_center(cancel_lbl);

    lv_obj_t *open = lv_button_create(panel);
    lv_obj_set_size(open, 150, 48);
    lv_obj_align(open, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(open, 12, 0);
    lv_obj_set_style_bg_color(open, lv_color_make(0xB4, 0x53, 0x09), 0);
    lv_obj_set_style_shadow_width(open, 0, 0);
    lv_obj_add_event_cb(open, confirm_not_recommended_open_cb, LV_EVENT_CLICKED, a);
    lv_obj_t *open_lbl = lv_label_create(open);
    lv_label_set_text(open_lbl, "OPEN");
    lv_obj_set_style_text_color(open_lbl, lv_color_white(), 0);
    lv_obj_center(open_lbl);

    lv_obj_move_foreground(a->not_recommended_confirm_overlay);
}

static void show_stream_opening_overlay(app_t *a, int idx)
{
    if (!a || idx < 0 || idx >= (int)a->mode_count) return;
    const cam_mode_t *m = &a->modes[idx];

    lv_obj_t *overlay = a->not_recommended_confirm_overlay;
    if (!overlay) {
        overlay = lv_obj_create(lv_scr_act());
        a->not_recommended_confirm_overlay = overlay;
        lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
        lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_scrollable(overlay, false);
        lv_obj_set_clickable(overlay, true);
    } else {
        ui_reset_touch_cache(a);
        lv_obj_clean(overlay);
    }

    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);

    lv_obj_t *title = ui_create_info_label(overlay, "Opening video stream...",
                                           lv_color_white(),
                                           &lv_font_montserrat_24);
    lv_obj_set_width(title, 920);
    lv_obj_set_height(title, 40);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -58);

    char display_note[224];
    ui_format_display_viewport_note(a, m, display_note, sizeof(display_note));

    char msg[384];
    snprintf(msg, sizeof(msg), "%s %ux%u @%.1f fps\n%s\nWaiting for the first frame",
             uvc_format_name(m->fmt), m->w, m->h, m->fps, display_note);
    lv_obj_t *body = ui_create_info_label(overlay, msg,
                                          lv_color_make(0xCB, 0xD5, 0xE1),
                                          &lv_font_montserrat_16);
    lv_obj_set_width(body, 920);
    lv_obj_set_height(body, 112);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 28);

    lv_obj_move_foreground(overlay);
    lv_obj_invalidate(overlay);
}

static void confirm_not_recommended_cancel_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !a) return;
    ui_clear_not_recommended_confirm(a);
    if (a->selected_mode >= 0 && a->selected_mode < (int)a->mode_count &&
        mode_start_requires_confirmation(&a->modes[a->selected_mode])) {
        ui_set_status(a, "Press START to confirm opening this not recommended mode");
    } else {
        ui_set_status(a, "Select mode and press START");
    }
}

static void confirm_not_recommended_open_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !a) return;
    const int idx = a->not_recommended_confirm_mode;
    if (idx < 0 || idx >= (int)a->mode_count) {
        ui_clear_not_recommended_confirm(a);
        return;
    }

    const cam_mode_t *m = &a->modes[idx];
    if (!mode_start_requires_confirmation(m)) {
        ui_clear_not_recommended_confirm(a);
        return;
    }
    if (!mode_is_safe(m)) {
        ui_clear_not_recommended_confirm(a);
        ui_set_status(a, "Selected mode needs too much USB memory");
        return;
    }

    a->not_recommended_confirmed_mode = idx;
    a->selected_mode = idx;
    show_stream_opening_overlay(a, idx);
    ui_commit_selected_mode(a);
}

static void list_item_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= (int)a->mode_count) return;
    if (a->check_running) return;
    // 不可启动的模式仍可点击，用于解释为什么不能选择.
    const char *unavailable = mode_unavailable_reason(&a->modes[idx]);
    if (unavailable && strcmp(unavailable, "not recommended") != 0) {
        if (strcmp(unavailable, "hardware unsupported") == 0) {
            ui_set_status(a, "This mode is hardware unsupported");
        } else {
            char msg[96];
            snprintf(msg, sizeof(msg), "Mode unavailable: %s", unavailable);
            ui_set_status(a, msg);
        }
        return;
    }

    // 清除所有项高亮
    a->not_recommended_confirmed_mode = -1;
    ui_set_selected_mode_visual(a, idx);

    // 高亮当前选中项
    a->selected_mode = idx;
    ui_update_info_panel(a);
    if (a->confirm_btn && ui_mode_start_allowed(a, idx)) {
        lv_obj_clear_state(a->confirm_btn, LV_STATE_DISABLED);
    }
    if (unavailable && strcmp(unavailable, "not recommended") == 0) {
        ui_set_status(a, "Press START to confirm opening this not recommended mode");
    } else {
        ui_set_status(a, "Select mode and press START");
    }
}

static void btn_confirm_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (a->check_running) return;
    if (a->selected_mode < 0 || a->selected_mode >= (int)a->mode_count) return;
    const cam_mode_t *selected = &a->modes[a->selected_mode];
    const char *unavailable = mode_unavailable_reason(selected);
    if (!ui_mode_start_allowed(a, a->selected_mode)) {
        if (unavailable && strcmp(unavailable, "hardware unsupported") == 0) {
            ui_set_status(a, "Pick a hardware supported mode to continue");
        } else {
            ui_set_status(a, "Pick a startable mode to continue");
        }
        return;
    }
    if (mode_start_requires_confirmation(selected) &&
        a->not_recommended_confirmed_mode != a->selected_mode) {
        show_not_recommended_confirm_dialog(a, a->selected_mode);
        return;
    }
    if (!mode_is_safe(selected)) {
        ui_set_status(a, "Selected mode needs too much USB memory");
        return;
    }
    show_stream_opening_overlay(a, a->selected_mode);
    ui_commit_selected_mode(a);
}

static void btn_check_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!a || a->check_running || a->mode_count == 0) return;

    a->check_running = true;
    if (a->confirm_btn) lv_obj_add_state(a->confirm_btn, LV_STATE_DISABLED);
    if (a->check_btn) lv_obj_add_state(a->check_btn, LV_STATE_DISABLED);
    if (a->status_lbl) lv_label_set_text(a->status_lbl, "Checking camera modes...");

    BaseType_t ok = xTaskCreatePinnedToCore(check_task_entry, "uvc_check", 8192, a,
                                            UVC_DECODE_TASK_PRIO, &a->check_task,
                                            tskNO_AFFINITY);
    if (ok != pdTRUE) {
        a->check_running = false;
        a->check_task = nullptr;
        if (a->confirm_btn) lv_obj_clear_state(a->confirm_btn, LV_STATE_DISABLED);
        if (a->check_btn) lv_obj_clear_state(a->check_btn, LV_STATE_DISABLED);
        if (a->status_lbl) lv_label_set_text(a->status_lbl, "CHECK task create failed");
    }
}

static void check_close_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !a || a->check_running) return;
    if (a->check_overlay) {
        ui_reset_touch_cache(a);
        lv_obj_delete(a->check_overlay);
        a->check_overlay = nullptr;
        a->check_list = nullptr;
        a->check_status_lbl = nullptr;
        a->check_close_btn = nullptr;
    }
    if (a->status_lbl) lv_label_set_text(a->status_lbl, "CHECK finished. Select mode and press START");
}

// REINIT 按钮：在选择/等待界面手动让 USB 摄像头掉电再上电，便于摄像头卡死时恢复。
static void btn_reinit_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !a) return;
    if (a->check_running) return;
    if (a->state == APP_STATE_STREAMING) {
        if (a->status_lbl) lv_label_set_text(a->status_lbl, "Stop the stream before REINIT");
        return;
    }
    if (a->status_lbl) lv_label_set_text(a->status_lbl, "REINIT: cycling USB power...");
    if (a->confirm_btn) lv_obj_add_state(a->confirm_btn, LV_STATE_DISABLED);
    if (a->check_btn)   lv_obj_add_state(a->check_btn, LV_STATE_DISABLED);
    if (a->adv_btn)     lv_obj_add_state(a->adv_btn, LV_STATE_DISABLED);
    if (a->adv_switch)  lv_obj_add_state(a->adv_switch, LV_STATE_DISABLED);
    app_request_usb_reinit_from_user(a);
}

// ===================================================================
// ADVANCED 控制面板
// ===================================================================
static lv_color_t kColPanelBorder = LV_COLOR_MAKE(0xE2, 0xE8, 0xF0);
static lv_color_t kColTitle       = LV_COLOR_MAKE(0x0F, 0x17, 0x2A);
static lv_color_t kColMuted       = LV_COLOR_MAKE(0x64, 0x74, 0x8B);
static lv_color_t kColAccent      = LV_COLOR_MAKE(0x25, 0x63, 0xEB);

static const char *adv_widget_kind(const cam_ctrl_desc_t *d, int *steps)
{
    int span = d->v_max - d->v_min;
    if (span <= 0) { *steps = 1; return "static"; }
    *steps = span + 1;
    if (d->v_min == 0 && d->v_max == 1) return "toggle";
    if (span >= 2 && span <= 6 && d->value_size == 4 &&
        d->entity_id == UVC_ENTITY_ID_EXTENSION_UNIT &&
        (d->selector == UVC_XU_CTRL_SPECIAL_EFFECT || d->selector == UVC_XU_CTRL_WB_MODE)) {
        return "stepper";
    }
    return "slider";
}

enum {
    ADV_STEP_TOGGLE = 0,
    ADV_STEP_DEC    = 1,
    ADV_STEP_INC    = 2,
};

static void *adv_step_user_data(int idx, int action)
{
    return (void *)(intptr_t)(idx * 4 + action + 1);
}

static bool adv_step_decode(void *user_data, int *idx, int *action)
{
    int code = (int)(intptr_t)user_data - 1;
    if (code < 0) return false;
    *idx = code / 4;
    *action = code % 4;
    return true;
}

static esp_err_t adv_apply_control_value(app_t *a, int idx, int32_t new_val, bool *deferred)
{
    if (deferred) *deferred = false;
    if (!a || idx < 0 || idx >= (int)CAM_ADV_CTRL_COUNT) return ESP_FAIL;
    cam_ctrl_desc_t *d = &a->adv_caps[idx];
    if (!d->supported) return ESP_FAIL;

    if (new_val < d->v_min) new_val = d->v_min;
    if (new_val > d->v_max) new_val = d->v_max;
    d->v_cur = new_val;

    esp_err_t err = ESP_FAIL;
    if (a->stream_hdl && !cam_ctrl_is_live_safe(d)) {
        if (deferred) *deferred = true;
    } else if (a->stream_hdl) {
        err = cam_ctrl_set_int(a->stream_hdl, d, d->v_cur);
    }
    d->pending_apply = (err != ESP_OK);
    return err;
}

static void adv_update_status(app_t *a, const cam_ctrl_desc_t *d, esp_err_t err, bool deferred)
{
    if (!a || !a->adv_status_lbl || !d) return;
    char buf[80];
    snprintf(buf, sizeof(buf), "%s = %ld%s", d->name, (long)d->v_cur,
             (err == ESP_OK) ? "" : (deferred ? " (next start)" : " (pending)"));
    lv_label_set_text(a->adv_status_lbl, buf);
}

static void adv_update_toggle_button(lv_obj_t *btn, const cam_ctrl_desc_t *d)
{
    if (!btn || !d) return;
    bool on = d->v_cur >= d->v_max;
    lv_color_t color = on ? lv_color_make(0x16, 0xA3, 0x4A)
                          : lv_color_make(0x94, 0xA3, 0xB8);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) lv_label_set_text(lbl, on ? "ON" : "OFF");
}

static void adv_update_stepper_label(lv_obj_t *stepper, const cam_ctrl_desc_t *d)
{
    if (!stepper || !d) return;
    lv_obj_t *lbl = lv_obj_get_child(stepper, 1);
    if (!lbl) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", (long)d->v_cur);
    lv_label_set_text(lbl, buf);
}

// 用户在面板里调整控件后回调：更新 v_cur，必要时下发或标记 pending。
static void adv_widget_changed_cb(lv_event_t *e)
{
    app_t *a = &s_app;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)CAM_ADV_CTRL_COUNT) return;
    cam_ctrl_desc_t *d = &a->adv_caps[idx];
    if (!d->supported) return;
    lv_obj_t *w = (lv_obj_t *)lv_event_get_target(e);

    int32_t new_val = d->v_cur;
    int steps = 0;
    const char *kind = adv_widget_kind(d, &steps);
    if (strcmp(kind, "switch") == 0) {
        new_val = lv_obj_has_state(w, LV_STATE_CHECKED) ? d->v_max : d->v_min;
    } else if (strcmp(kind, "dropdown") == 0) {
        uint16_t sel = lv_dropdown_get_selected(w);
        new_val = d->v_min + (int32_t)sel;
    } else {
        // slider 取 0..span，再映射到 [v_min, v_max]
        int32_t s = (int32_t)lv_slider_get_value(w);
        new_val = d->v_min + s;
    }
    if (new_val < d->v_min) new_val = d->v_min;
    if (new_val > d->v_max) new_val = d->v_max;
    d->v_cur = new_val;

    // 立即下发；若流未打开或该项不适合 live 修改，则标记 pending，下次 stream_start 前回放。
    esp_err_t err = ESP_FAIL;
    bool deferred = false;
    if (a->stream_hdl && !cam_ctrl_is_live_safe(d)) {
        deferred = true;
    } else if (a->stream_hdl) {
        err = cam_ctrl_set_int(a->stream_hdl, d, d->v_cur);
    }
    if (err != ESP_OK) {
        d->pending_apply = true;
    } else {
        d->pending_apply = false;
    }

    if (a->adv_status_lbl) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%s = %ld%s", d->name, (long)d->v_cur,
                 (err == ESP_OK) ? "" : (deferred ? " (next start)" : " (pending)"));
        lv_label_set_text(a->adv_status_lbl, buf);
    }
}

static void adv_step_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    app_t *a = &s_app;
    int idx = -1;
    int action = -1;
    if (!adv_step_decode(lv_event_get_user_data(e), &idx, &action)) return;
    if (idx < 0 || idx >= (int)CAM_ADV_CTRL_COUNT) return;
    cam_ctrl_desc_t *d = &a->adv_caps[idx];
    if (!d->supported) return;

    int32_t new_val = d->v_cur;
    if (action == ADV_STEP_TOGGLE) {
        new_val = (d->v_cur >= d->v_max) ? d->v_min : d->v_max;
    } else if (action == ADV_STEP_DEC) {
        new_val = d->v_cur - 1;
    } else if (action == ADV_STEP_INC) {
        new_val = d->v_cur + 1;
    }

    bool deferred = false;
    esp_err_t err = adv_apply_control_value(a, idx, new_val, &deferred);
    lv_obj_t *target = (lv_obj_t *)lv_event_get_target(e);
    if (action == ADV_STEP_TOGGLE) {
        adv_update_toggle_button(target, d);
    } else {
        adv_update_stepper_label(lv_obj_get_parent(target), d);
    }
    adv_update_status(a, d, err, deferred);
}

static void adv_switch_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED) return;
    if (!a || !a->adv_switch) return;
    if (!adv_any_supported(a)) {
        a->adv_live_enabled = false;
        lv_obj_clear_state(a->adv_switch, LV_STATE_CHECKED);
        if (a->status_lbl) lv_label_set_text(a->status_lbl, "Camera descriptors report no advanced controls");
        ui_update_advanced_button(a);
        return;
    }
    a->adv_live_enabled = lv_obj_has_state(a->adv_switch, LV_STATE_CHECKED);
    if (a->status_lbl) {
        lv_label_set_text(a->status_lbl,
                          a->adv_live_enabled ?
                          "Advanced sidebar will stay visible in video" :
                          "Advanced sidebar disabled; use Configuration before START");
    }
    ui_update_advanced_button(a);
}

static void adv_option_click_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !a || !a->adv_switch) return;
    if (!adv_any_supported(a)) return;
    if (lv_obj_has_state(a->adv_switch, LV_STATE_CHECKED)) {
        lv_obj_clear_state(a->adv_switch, LV_STATE_CHECKED);
    } else {
        lv_obj_add_state(a->adv_switch, LV_STATE_CHECKED);
    }
    a->adv_live_enabled = lv_obj_has_state(a->adv_switch, LV_STATE_CHECKED);
    ui_update_advanced_button(a);
}

static void adv_overlay_bg_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !a) return;
    if (lv_event_get_target(e) != a->adv_overlay) return; // 只响应背景点击
    if (a->adv_overlay) {
        ui_reset_touch_cache(a);
        lv_obj_delete(a->adv_overlay);
        a->adv_overlay = nullptr;
        a->adv_panel = nullptr;
        a->adv_status_lbl = nullptr;
    }
}

static void adv_close_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !a) return;
    if (a->adv_overlay) {
        ui_reset_touch_cache(a);
        lv_obj_delete(a->adv_overlay);
        a->adv_overlay = nullptr;
        a->adv_panel = nullptr;
        a->adv_status_lbl = nullptr;
    }
}

static void adv_build_row(app_t *a, lv_obj_t *parent, int idx)
{
    cam_ctrl_desc_t *d = &a->adv_caps[idx];
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, kColPanelBorder, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 7, 0);
    lv_obj_set_style_pad_row(row, 3, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(row, false);

    char hdr[96];
    snprintf(hdr, sizeof(hdr), "%s  [%ld..%ld]  cur=%ld",
             d->name, (long)d->v_min, (long)d->v_max, (long)d->v_cur);
    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, hdr);
    lv_obj_set_style_text_color(title, kColTitle, 0);

    int steps = 0;
    const char *kind = adv_widget_kind(d, &steps);

    if (strcmp(kind, "toggle") == 0) {
        lv_obj_t *btn = lv_button_create(row);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, 40);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, adv_step_click_cb, LV_EVENT_CLICKED,
                            adv_step_user_data(idx, ADV_STEP_TOGGLE));
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_center(lbl);
        adv_update_toggle_button(btn, d);
        lv_obj_set_height(row, 82);
    } else if (strcmp(kind, "stepper") == 0) {
        lv_obj_t *stepper = lv_obj_create(row);
        lv_obj_set_width(stepper, lv_pct(100));
        lv_obj_set_height(stepper, 42);
        lv_obj_set_style_bg_opa(stepper, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(stepper, 0, 0);
        lv_obj_set_style_pad_all(stepper, 0, 0);
        lv_obj_set_style_pad_column(stepper, 8, 0);
        lv_obj_set_scrollable(stepper, false);
        lv_obj_set_layout(stepper, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(stepper, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(stepper, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *dec = lv_button_create(stepper);
        lv_obj_set_size(dec, 76, 40);
        lv_obj_set_style_radius(dec, 10, 0);
        lv_obj_set_style_bg_color(dec, lv_color_make(0xE2, 0xE8, 0xF0), 0);
        lv_obj_set_style_shadow_width(dec, 0, 0);
        lv_obj_add_event_cb(dec, adv_step_click_cb, LV_EVENT_CLICKED,
                            adv_step_user_data(idx, ADV_STEP_DEC));
        lv_obj_t *dec_lbl = lv_label_create(dec);
        lv_label_set_text(dec_lbl, "-");
        lv_obj_set_style_text_color(dec_lbl, kColTitle, 0);
        lv_obj_center(dec_lbl);

        lv_obj_t *val_lbl = lv_label_create(stepper);
        lv_obj_set_width(val_lbl, 150);
        lv_obj_set_style_text_align(val_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(val_lbl, kColTitle, 0);
        adv_update_stepper_label(stepper, d);

        lv_obj_t *inc = lv_button_create(stepper);
        lv_obj_set_size(inc, 76, 40);
        lv_obj_set_style_radius(inc, 10, 0);
        lv_obj_set_style_bg_color(inc, kColAccent, 0);
        lv_obj_set_style_shadow_width(inc, 0, 0);
        lv_obj_add_event_cb(inc, adv_step_click_cb, LV_EVENT_CLICKED,
                            adv_step_user_data(idx, ADV_STEP_INC));
        lv_obj_t *inc_lbl = lv_label_create(inc);
        lv_label_set_text(inc_lbl, "+");
        lv_obj_set_style_text_color(inc_lbl, lv_color_white(), 0);
        lv_obj_center(inc_lbl);

        lv_obj_set_height(row, 84);
    } else {
        // slider: range 0..(span)
        lv_obj_t *sl = lv_slider_create(row);
        lv_obj_set_width(sl, lv_pct(100));
        int32_t span = d->v_max - d->v_min;
        if (span < 1) span = 1;
        lv_slider_set_range(sl, 0, span);
        int32_t pos = d->v_cur - d->v_min;
        if (pos < 0) pos = 0;
        if (pos > span) pos = span;
        lv_slider_set_value(sl, pos, LV_ANIM_OFF);
        lv_obj_add_event_cb(sl, adv_widget_changed_cb, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)idx);
        lv_obj_set_height(row, 60);
    }
}

static void btn_advanced_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !a) return;
    if (a->check_running) return;
    int support_cnt = 0;
    for (int i = 0; i < (int)CAM_ADV_CTRL_COUNT; i++) {
        if (a->adv_caps[i].supported) support_cnt++;
    }

    // 已存在则刷新
    if (a->adv_overlay) {
        ui_reset_touch_cache(a);
        lv_obj_delete(a->adv_overlay);
        a->adv_overlay = nullptr;
        a->adv_panel = nullptr;
        a->adv_status_lbl = nullptr;
    }

    lv_obj_t *scr = lv_scr_act();
    a->adv_overlay = lv_obj_create(scr);
    lv_obj_set_size(a->adv_overlay, lv_pct(100), lv_pct(100));
    lv_obj_align(a->adv_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(a->adv_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(a->adv_overlay, LV_OPA_40, 0);
    lv_obj_set_style_border_width(a->adv_overlay, 0, 0);
    lv_obj_set_style_radius(a->adv_overlay, 0, 0);
    lv_obj_set_style_pad_all(a->adv_overlay, 0, 0);
    lv_obj_set_scrollable(a->adv_overlay, false);
    lv_obj_set_clickable(a->adv_overlay, true);
    lv_obj_add_event_cb(a->adv_overlay, adv_overlay_bg_cb, LV_EVENT_CLICKED, a);

    a->adv_panel = lv_obj_create(a->adv_overlay);
    lv_obj_set_size(a->adv_panel, 380, PANEL_LOGIC_H - 28);
    lv_obj_align(a->adv_panel, LV_ALIGN_RIGHT_MID, -14, 0);
    ui_set_panel_style(a->adv_panel, lv_color_white(), 16);
    lv_obj_set_style_pad_all(a->adv_panel, 12, 0);
    lv_obj_set_style_pad_row(a->adv_panel, 6, 0);
    lv_obj_set_layout(a->adv_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(a->adv_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->adv_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(a->adv_panel, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *title = lv_label_create(a->adv_panel);
    lv_label_set_text(title, "Camera advanced");
    lv_obj_set_style_text_color(title, kColTitle, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_height(title, 30);

    a->adv_status_lbl = lv_label_create(a->adv_panel);
    lv_label_set_long_mode(a->adv_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(a->adv_status_lbl, lv_pct(100));
    lv_obj_set_style_text_color(a->adv_status_lbl, kColMuted, 0);
    lv_obj_set_height(a->adv_status_lbl, 24);
    if (support_cnt == 0) {
        lv_label_set_text(a->adv_status_lbl, "No configurable controls detected for this device");
    } else if (a->stream_hdl) {
        lv_label_set_text(a->adv_status_lbl, "Adjust to send live");
    } else {
        lv_label_set_text(a->adv_status_lbl, "Changes apply on next START");
    }

    for (int i = 0; i < (int)CAM_ADV_CTRL_COUNT; i++) {
        if (a->adv_caps[i].supported) adv_build_row(a, a->adv_panel, i);
    }

    lv_obj_t *close_btn = lv_button_create(a->adv_panel);
    lv_obj_set_size(close_btn, lv_pct(100), 38);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_set_style_bg_color(close_btn, kColAccent, 0);
    lv_obj_add_event_cb(close_btn, adv_close_cb, LV_EVENT_CLICKED, a);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "CLOSE");
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_center(cl);

    lv_obj_move_foreground(a->adv_overlay);
}

void ui_update_advanced_button(app_t *a)
{
    if (!a) return;
    bool any = adv_any_supported(a);
    if (!any) {
        a->adv_live_enabled = false;
        if (a->adv_switch) lv_obj_clear_state(a->adv_switch, LV_STATE_CHECKED);
    }
    if (a->adv_switch) {
        if (any) lv_obj_clear_state(a->adv_switch, LV_STATE_DISABLED);
        else     lv_obj_add_state(a->adv_switch, LV_STATE_DISABLED);
        if (a->adv_live_enabled) lv_obj_add_state(a->adv_switch, LV_STATE_CHECKED);
        else                     lv_obj_clear_state(a->adv_switch, LV_STATE_CHECKED);
    }
    bool probed = a->adv_caps_probed;
    if (a->adv_btn) {
        if (probed) lv_obj_clear_state(a->adv_btn, LV_STATE_DISABLED);
        else        lv_obj_add_state(a->adv_btn, LV_STATE_DISABLED);
    }
    if (a->adv_lbl) {
        if (!probed) {
            lv_label_set_text(a->adv_lbl,
                              adv_any_candidate(a) ?
                                  "Advanced: verifying..." :
                                  "Advanced: unavailable");
        } else if (!any) {
            lv_label_set_text(a->adv_lbl, "Advanced: no controls");
        } else if (a->adv_live_enabled) {
            lv_label_set_text(a->adv_lbl, "Advanced: show in video");
        } else {
            lv_label_set_text(a->adv_lbl, "Advanced: configure before START");
        }
    }
}

void ui_build_stream_overlay(app_t *a)
{
    if (!a) return;
    if (!otool_lvgl_port_lock(100)) return;

    if (a->stream_overlay) {
        ui_reset_touch_cache(a);
        lv_obj_delete(a->stream_overlay);
        a->stream_overlay = nullptr;
        a->stream_adv_panel = nullptr;
        a->stream_audio_panel = nullptr;
        a->stream_status_lbl = nullptr;
    }
    if (a->sel_screen) {
        lv_obj_set_hidden(a->sel_screen, true);
    }

    lv_obj_t *scr = lv_scr_act();
    a->stream_overlay = lv_obj_create(scr);
    lv_obj_set_size(a->stream_overlay, lv_pct(100), lv_pct(100));
    lv_obj_align(a->stream_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(a->stream_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(a->stream_overlay, 0, 0);
    lv_obj_set_style_radius(a->stream_overlay, 0, 0);
    lv_obj_set_style_pad_all(a->stream_overlay, 0, 0);
    lv_obj_set_scrollable(a->stream_overlay, false);

    lv_obj_t *badge = lv_obj_create(a->stream_overlay);
    lv_obj_set_size(badge, 238, 52);
    lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 18, 16);
    lv_obj_set_style_bg_color(badge, lv_color_make(0x02, 0x08, 0x12), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_70, 0);
    lv_obj_set_style_border_width(badge, 1, 0);
    lv_obj_set_style_border_color(badge, lv_color_make(0x22, 0xD3, 0xEE), 0);
    lv_obj_set_style_radius(badge, 14, 0);
    lv_obj_set_style_pad_all(badge, 10, 0);
    lv_obj_set_scrollable(badge, false);

    char badge_text[80];
    snprintf(badge_text, sizeof(badge_text), "LIVE%s%s",
             a->uac_selected ? " / UAC" : "",
             a->adv_live_enabled ? " / ADV" : "");
    a->stream_status_lbl = lv_label_create(badge);
    lv_label_set_text(a->stream_status_lbl, badge_text);
    lv_obj_set_style_text_color(a->stream_status_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(a->stream_status_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(a->stream_status_lbl);

    if (a->uac_selected) {
        a->stream_audio_panel = lv_obj_create(a->stream_overlay);
        lv_obj_set_size(a->stream_audio_panel, 360, 72);
        lv_obj_align(a->stream_audio_panel, LV_ALIGN_BOTTOM_LEFT, 18, -18);
        lv_obj_set_style_bg_color(a->stream_audio_panel, lv_color_make(0x02, 0x08, 0x12), 0);
        lv_obj_set_style_bg_opa(a->stream_audio_panel, LV_OPA_60, 0);
        lv_obj_set_style_border_width(a->stream_audio_panel, 1, 0);
        lv_obj_set_style_border_color(a->stream_audio_panel, lv_color_make(0x34, 0xD3, 0x99), 0);
        lv_obj_set_style_radius(a->stream_audio_panel, 16, 0);
        lv_obj_set_scrollable(a->stream_audio_panel, false);
        lv_obj_t *audio_lbl = lv_label_create(a->stream_audio_panel);
        lv_label_set_text(audio_lbl, "UAC audio active\nRecord/play panel remains on video");
        lv_obj_set_style_text_color(audio_lbl, lv_color_make(0xD1, 0xFA, 0xE5), 0);
        lv_obj_align(audio_lbl, LV_ALIGN_LEFT_MID, 14, 0);
    }

    if (a->adv_live_enabled && adv_any_supported(a)) {
        a->stream_adv_panel = lv_obj_create(a->stream_overlay);
        lv_obj_set_size(a->stream_adv_panel, 380, PANEL_LOGIC_H - 28);
        lv_obj_align(a->stream_adv_panel, LV_ALIGN_RIGHT_MID, -14, 0);
        ui_set_panel_style(a->stream_adv_panel, lv_color_white(), 16);
        lv_obj_set_style_bg_opa(a->stream_adv_panel, LV_OPA_90, 0);
        lv_obj_set_style_pad_all(a->stream_adv_panel, 12, 0);
        lv_obj_set_style_pad_row(a->stream_adv_panel, 6, 0);
        lv_obj_set_layout(a->stream_adv_panel, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(a->stream_adv_panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(a->stream_adv_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_scrollbar_mode(a->stream_adv_panel, LV_SCROLLBAR_MODE_AUTO);

        lv_obj_t *title = lv_label_create(a->stream_adv_panel);
        lv_label_set_text(title, "Camera advanced");
        lv_obj_set_style_text_color(title, kColTitle, 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_set_height(title, 30);

        a->adv_status_lbl = lv_label_create(a->stream_adv_panel);
        lv_label_set_long_mode(a->adv_status_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(a->adv_status_lbl, lv_pct(100));
        lv_obj_set_height(a->adv_status_lbl, 24);
        lv_obj_set_style_text_color(a->adv_status_lbl, kColMuted, 0);
        lv_label_set_text(a->adv_status_lbl, "Live control: changes are verified by GET_CUR");

        for (int i = 0; i < (int)CAM_ADV_CTRL_COUNT; i++) {
            if (a->adv_caps[i].supported) adv_build_row(a, a->stream_adv_panel, i);
        }
    }

    lv_obj_move_foreground(a->stream_overlay);
    lv_obj_invalidate(a->stream_overlay);
    otool_lvgl_port_unlock();
}

void ui_refresh_stream_overlay(app_t *a)
{
    if (!a || !a->stream_lvgl_ui || !a->stream_overlay) return;
    if (!otool_lvgl_port_lock(0)) return;
    lv_obj_move_foreground(a->stream_overlay);
    if (a->stream_status_lbl) {
        lv_obj_t *badge = lv_obj_get_parent(a->stream_status_lbl);
        if (badge) lv_obj_invalidate(badge);
    }
    if (a->stream_audio_panel) lv_obj_invalidate(a->stream_audio_panel);
    if (a->stream_adv_panel) lv_obj_invalidate(a->stream_adv_panel);
    otool_lvgl_port_unlock();
}

void ui_destroy_stream_overlay(app_t *a)
{
    if (!a) return;
    if (!otool_lvgl_port_lock(100)) return;
    if (a->stream_overlay) {
        ui_reset_touch_cache(a);
        lv_obj_delete(a->stream_overlay);
    }
    a->stream_overlay = nullptr;
    a->stream_adv_panel = nullptr;
    a->stream_audio_panel = nullptr;
    a->stream_status_lbl = nullptr;
    a->adv_status_lbl = nullptr;
    if (a->sel_screen) {
        lv_obj_set_hidden(a->sel_screen, false);
    }
    otool_lvgl_port_unlock();
}

void ui_style_mode_item(lv_obj_t *item, bool selected)
{
    // 白色简约：选中蓝边+浅蓝背景，未选中灰边白底
    lv_obj_set_style_radius(item, 10, 0);
    lv_obj_set_style_border_width(item, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(item,
        selected ? lv_color_make(0x3B, 0x82, 0xF6) : lv_color_make(0xE2, 0xE8, 0xF0), 0);
    lv_obj_set_style_bg_color(item,
        selected ? lv_color_make(0xEF, 0xF6, 0xFF) : lv_color_make(0xFF, 0xFF, 0xFF), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(item, selected ? 10 : 0, 0);
    lv_obj_set_style_shadow_color(item, lv_color_make(0x3B, 0x82, 0xF6), 0);
    lv_obj_set_style_shadow_opa(item, selected ? LV_OPA_10 : LV_OPA_0, 0);
}

void ui_populate_mode_list(app_t *a)
{
    if (!a->list_obj) return;
    ui_reset_touch_cache(a);
    ui_clear_not_recommended_confirm(a);
    lv_obj_clean(a->list_obj);

    for (int i = 0; i < (int)a->mode_count; i++) {
        const cam_mode_t *m = &a->modes[i];
        const char *unavailable = mode_unavailable_reason(m);
        const bool selectable = (unavailable == nullptr) ||
                                (strcmp(unavailable, "not recommended") == 0);

        char res_buf[64];
        char fps_buf[24];
        lv_obj_t *btn = lv_button_create(a->list_obj);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, 66);
        lv_obj_set_style_pad_left(btn, 16, 0);
        lv_obj_set_style_pad_right(btn, 16, 0);
        lv_obj_set_style_pad_top(btn, 8, 0);
        lv_obj_set_style_pad_bottom(btn, 8, 0);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, list_item_cb, LV_EVENT_CLICKED, a);

        if (selectable) {
            snprintf(res_buf, sizeof(res_buf), "%ux%u", m->w, m->h);
        } else {
            snprintf(res_buf, sizeof(res_buf), "%ux%u  (%s)", m->w, m->h, unavailable);
        }
        lv_obj_t *res_lbl = lv_label_create(btn);
        lv_label_set_text(res_lbl, res_buf);
        lv_obj_set_style_text_font(res_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(res_lbl,
            selectable ? lv_color_make(0x1E, 0x29, 0x3B)
                       : lv_color_make(0x94, 0xA3, 0xB8), 0);
        lv_obj_align(res_lbl, LV_ALIGN_LEFT_MID, 0, -10);

        snprintf(fps_buf, sizeof(fps_buf), "%.1f fps", m->fps);
        lv_obj_t *fps_lbl = lv_label_create(btn);
        lv_label_set_text(fps_lbl, fps_buf);
        lv_obj_set_style_text_color(fps_lbl,
            selectable ? lv_color_make(0x3B, 0x82, 0xF6)
                       : lv_color_make(0x94, 0xA3, 0xB8), 0);
        lv_obj_align(fps_lbl, LV_ALIGN_RIGHT_MID, 0, -10);

        lv_obj_t *fmt_lbl = lv_label_create(btn);
        lv_label_set_text(fmt_lbl, uvc_format_name(m->fmt));
        lv_obj_set_style_text_color(fmt_lbl, lv_color_make(0x64, 0x74, 0x8B), 0);
        lv_obj_align(fmt_lbl, LV_ALIGN_LEFT_MID, 0, 14);

        if (i == a->selected_mode) {
            ui_style_mode_item(btn, true);
        } else {
            ui_style_mode_item(btn, false);
        }
        if (!selectable) {
            // 视觉上变灰, 仍保留点击以作为 "why disabled" 提示 (list_item_cb 里处理).
            lv_obj_set_style_bg_color(btn, lv_color_make(0xF1, 0xF5, 0xF9), 0);
            lv_obj_set_style_border_color(btn, lv_color_make(0xCB, 0xD5, 0xE1), 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
        }
    }
}

void ui_clear_mode_list(app_t *a)
{
    if (!a->list_obj) return;
    ui_reset_touch_cache(a);
    ui_clear_not_recommended_confirm(a);
    lv_obj_clean(a->list_obj);
}

void ui_set_status(app_t *a, const char *txt)
{
    if (!a->status_lbl) return;
    if (otool_lvgl_port_lock(50)) {
        lv_label_set_text(a->status_lbl, txt);
        otool_lvgl_port_unlock();
    }
}

void ui_build_selection(app_t *a)
{
    if (!otool_lvgl_port_lock(0)) { ESP_LOGE(TAG, "lvgl lock fail"); return; }

    lv_obj_t *scr = lv_scr_act();
    ui_reset_touch_cache(a);
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_make(0xEA, 0xF4, 0xFF), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    const int32_t margin = 22;
    const int32_t gap = 18;
    const int32_t bottom_h = 72;
    const int32_t content_w = PANEL_LOGIC_W - margin * 2;
    const int32_t content_h = PANEL_LOGIC_H - margin * 2 - bottom_h - gap;
    const int32_t right_w = (content_w - gap) / 4;
    const int32_t left_w = content_w - right_w - gap;

    a->sel_screen = lv_obj_create(scr);
    lv_obj_set_size(a->sel_screen, PANEL_LOGIC_W, PANEL_LOGIC_H);
    lv_obj_align(a->sel_screen, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(a->sel_screen, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(a->sel_screen, 0, 0);
    lv_obj_set_style_pad_all(a->sel_screen, 0, 0);
    lv_obj_set_scrollable(a->sel_screen, false);

    lv_obj_t *left_panel = lv_obj_create(a->sel_screen);
    lv_obj_set_size(left_panel, left_w, content_h);
    lv_obj_align(left_panel, LV_ALIGN_TOP_LEFT, margin, margin);
    ui_set_panel_style(left_panel, lv_color_white(), 22);
    lv_obj_set_style_pad_all(left_panel, 22, 0);

    lv_obj_t *accent_bar = lv_obj_create(left_panel);
    lv_obj_set_size(accent_bar, 5, 42);
    lv_obj_align(accent_bar, LV_ALIGN_TOP_LEFT, 0, 2);
    lv_obj_set_style_bg_color(accent_bar, lv_color_make(0x25, 0x63, 0xEB), 0);
    lv_obj_set_style_border_width(accent_bar, 0, 0);
    lv_obj_set_style_radius(accent_bar, 3, 0);
    lv_obj_set_clickable(accent_bar, false);

    a->title_lbl = lv_label_create(left_panel);
    lv_label_set_text(a->title_lbl, "OTool Tab5 UVC Monitor");
    lv_obj_set_style_text_color(a->title_lbl, lv_color_make(0x0F, 0x17, 0x2A), 0);
    lv_obj_set_style_text_font(a->title_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(a->title_lbl, LV_ALIGN_TOP_LEFT, 18, 0);

    lv_obj_t *sub_lbl = lv_label_create(left_panel);
    lv_label_set_text(sub_lbl, "Descriptor order is preserved. Pick one mode or run CHECK first.");
    lv_obj_set_width(sub_lbl, left_w - 44 - 240);
    lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(sub_lbl, lv_color_make(0x64, 0x74, 0x8B), 0);
    lv_obj_align(sub_lbl, LV_ALIGN_TOP_LEFT, 18, 34);

    lv_obj_t *version_lbl = lv_label_create(left_panel);
    lv_label_set_text(version_lbl, TAB5_APP_VERSION_BUILD);
    lv_obj_set_width(version_lbl, 220);
    lv_label_set_long_mode(version_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(version_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(version_lbl, lv_color_make(0x94, 0xA3, 0xB8), 0);
    lv_obj_align(version_lbl, LV_ALIGN_TOP_RIGHT, -18, 34);

    a->list_obj = lv_obj_create(left_panel);
    lv_obj_set_size(a->list_obj, left_w - 44, content_h - 104);
    lv_obj_align(a->list_obj, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(a->list_obj, lv_color_make(0xF8, 0xFA, 0xFC), 0);
    lv_obj_set_style_bg_opa(a->list_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(a->list_obj, 1, 0);
    lv_obj_set_style_border_color(a->list_obj, lv_color_make(0xE2, 0xE8, 0xF0), 0);
    lv_obj_set_style_radius(a->list_obj, 16, 0);
    lv_obj_set_style_pad_left(a->list_obj, 16, 0);
    lv_obj_set_style_pad_right(a->list_obj, 16, 0);
    lv_obj_set_style_pad_top(a->list_obj, 14, 0);
    lv_obj_set_style_pad_bottom(a->list_obj, 14, 0);
    lv_obj_set_style_pad_row(a->list_obj, 10, 0);
    lv_obj_set_scrollbar_mode(a->list_obj, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(a->list_obj, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(a->list_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->list_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *right_panel = lv_obj_create(a->sel_screen);
    lv_obj_set_size(right_panel, right_w, content_h);
    lv_obj_align(right_panel, LV_ALIGN_TOP_RIGHT, -margin, margin);
    ui_set_panel_style(right_panel, lv_color_white(), 22);
    lv_obj_set_style_pad_all(right_panel, 18, 0);
    lv_obj_set_layout(right_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(right_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *info_title = ui_create_info_label(right_panel, "UVC Info", lv_color_make(0x0F, 0x17, 0x2A), &lv_font_montserrat_24);
    lv_obj_set_height(info_title, 34);
    a->info_mode_lbl = ui_create_info_label(right_panel, "Selected\nNo mode", lv_color_make(0x1E, 0x29, 0x3B), nullptr);
    lv_obj_set_height(a->info_mode_lbl, 68);
    a->info_device_lbl = ui_create_info_label(right_panel, "Device\nwaiting", lv_color_make(0x47, 0x55, 0x69), nullptr);
    lv_obj_set_height(a->info_device_lbl, 56);
    a->info_formats_lbl = ui_create_info_label(right_panel, "Formats\nwaiting", lv_color_make(0x47, 0x55, 0x69), nullptr);
    lv_obj_set_height(a->info_formats_lbl, 80);
    a->info_audio_lbl = ui_create_info_label(right_panel, "Audio: no UAC / off", lv_color_make(0x47, 0x55, 0x69), nullptr);
    lv_obj_set_height(a->info_audio_lbl, 26);

    lv_obj_t *uac_row = lv_obj_create(right_panel);
    lv_obj_set_width(uac_row, lv_pct(100));
    lv_obj_set_height(uac_row, 74);
    lv_obj_set_style_bg_color(uac_row, lv_color_make(0xF8, 0xFA, 0xFC), 0);
    lv_obj_set_style_bg_opa(uac_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(uac_row, 1, 0);
    lv_obj_set_style_border_color(uac_row, lv_color_make(0xE2, 0xE8, 0xF0), 0);
    lv_obj_set_style_pad_all(uac_row, 12, 0);
    lv_obj_set_style_radius(uac_row, 14, 0);
    lv_obj_set_scrollable(uac_row, false);
    lv_obj_set_clickable(uac_row, true);
    lv_obj_add_event_cb(uac_row, uac_option_click_cb, LV_EVENT_CLICKED, a);

    a->uac_lbl = lv_label_create(uac_row);
    lv_obj_set_width(a->uac_lbl, right_w - 128);
    lv_label_set_long_mode(a->uac_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(a->uac_lbl, lv_color_make(0x37, 0x41, 0x51), 0);
    lv_obj_align(a->uac_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(a->uac_lbl, "Audio: no UAC");

    a->uac_switch = lv_switch_create(uac_row);
    lv_obj_set_size(a->uac_switch, 78, 36);
    lv_obj_align(a->uac_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_clickable(a->uac_switch, true);
    lv_obj_set_event_bubble(a->uac_switch, false);
    lv_obj_add_event_cb(a->uac_switch, uac_switch_cb, LV_EVENT_VALUE_CHANGED, a);
    lv_obj_t *uac_switch_hit = lv_obj_create(uac_row);
    lv_obj_set_size(uac_switch_hit, 96, 52);
    lv_obj_align(uac_switch_hit, LV_ALIGN_RIGHT_MID, 4, 0);
    lv_obj_set_style_bg_opa(uac_switch_hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(uac_switch_hit, 0, 0);
    lv_obj_set_style_pad_all(uac_switch_hit, 0, 0);
    lv_obj_set_scrollable(uac_switch_hit, false);
    lv_obj_set_event_bubble(uac_switch_hit, false);
    lv_obj_set_clickable(uac_switch_hit, true);
    lv_obj_add_event_cb(uac_switch_hit, uac_option_click_cb, LV_EVENT_CLICKED, a);

    // ADVANCED：开关决定视频阶段是否常驻显示右侧参数侧栏；Configuration 用于进入前调整。
    a->adv_container = lv_obj_create(right_panel);
    lv_obj_set_width(a->adv_container, lv_pct(100));
    lv_obj_set_height(a->adv_container, 154);
    lv_obj_set_style_bg_color(a->adv_container, lv_color_make(0xF8, 0xFA, 0xFC), 0);
    lv_obj_set_style_bg_opa(a->adv_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(a->adv_container, 1, 0);
    lv_obj_set_style_border_color(a->adv_container, lv_color_make(0xE2, 0xE8, 0xF0), 0);
    lv_obj_set_style_pad_all(a->adv_container, 10, 0);
    lv_obj_set_style_radius(a->adv_container, 14, 0);
    lv_obj_set_scrollable(a->adv_container, false);

    lv_obj_t *adv_toggle_row = lv_obj_create(a->adv_container);
    lv_obj_set_width(adv_toggle_row, lv_pct(100));
    lv_obj_set_height(adv_toggle_row, 48);
    lv_obj_set_style_bg_opa(adv_toggle_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(adv_toggle_row, 0, 0);
    lv_obj_set_style_pad_all(adv_toggle_row, 0, 0);
    lv_obj_set_scrollable(adv_toggle_row, false);
    lv_obj_set_clickable(adv_toggle_row, true);
    lv_obj_add_event_cb(adv_toggle_row, adv_option_click_cb, LV_EVENT_CLICKED, a);

    a->adv_lbl = lv_label_create(adv_toggle_row);
    lv_obj_set_width(a->adv_lbl, right_w - 128);
    lv_label_set_long_mode(a->adv_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(a->adv_lbl, lv_color_make(0x37, 0x41, 0x51), 0);
    lv_obj_align(a->adv_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(a->adv_lbl, "Advanced: unavailable");

    a->adv_switch = lv_switch_create(adv_toggle_row);
    lv_obj_set_size(a->adv_switch, 78, 36);
    lv_obj_align(a->adv_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_clickable(a->adv_switch, true);
    lv_obj_set_event_bubble(a->adv_switch, false);
    lv_obj_add_state(a->adv_switch, LV_STATE_DISABLED);
    lv_obj_add_event_cb(a->adv_switch, adv_switch_cb, LV_EVENT_VALUE_CHANGED, a);
    lv_obj_t *adv_switch_hit = lv_obj_create(adv_toggle_row);
    lv_obj_set_size(adv_switch_hit, 96, 52);
    lv_obj_align(adv_switch_hit, LV_ALIGN_RIGHT_MID, 4, 0);
    lv_obj_set_style_bg_opa(adv_switch_hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(adv_switch_hit, 0, 0);
    lv_obj_set_style_pad_all(adv_switch_hit, 0, 0);
    lv_obj_set_scrollable(adv_switch_hit, false);
    lv_obj_set_event_bubble(adv_switch_hit, false);
    lv_obj_set_clickable(adv_switch_hit, true);
    lv_obj_add_event_cb(adv_switch_hit, adv_option_click_cb, LV_EVENT_CLICKED, a);

    lv_obj_t *adv_scope_lbl = lv_label_create(a->adv_container);
    lv_obj_set_width(adv_scope_lbl, right_w - 40);
    lv_label_set_long_mode(adv_scope_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(adv_scope_lbl, lv_color_make(0xB4, 0x53, 0x09), 0);
    lv_obj_align(adv_scope_lbl, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_label_set_text(adv_scope_lbl, "Custom UVC firmware only.\nStandard cameras unsupported.");

    a->adv_btn = lv_button_create(a->adv_container);
    lv_obj_set_width(a->adv_btn, lv_pct(100));
    lv_obj_set_height(a->adv_btn, 44);
    lv_obj_align(a->adv_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(a->adv_btn, 12, 0);
    lv_obj_set_style_bg_color(a->adv_btn, lv_color_make(0x0E, 0x76, 0x90), 0);
    lv_obj_set_style_bg_color(a->adv_btn, lv_color_make(0xCB, 0xD5, 0xE1), LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(a->adv_btn, 0, 0);
    lv_obj_add_state(a->adv_btn, LV_STATE_DISABLED);
    lv_obj_add_event_cb(a->adv_btn, btn_advanced_cb, LV_EVENT_CLICKED, a);
    lv_obj_t *adv_cfg_lbl = lv_label_create(a->adv_btn);
    lv_label_set_text(adv_cfg_lbl, "CONFIGURATION");
    lv_obj_set_style_text_color(adv_cfg_lbl, lv_color_white(), 0);
    lv_obj_center(adv_cfg_lbl);

    lv_obj_t *ftr = lv_obj_create(a->sel_screen);
    lv_obj_set_size(ftr, content_w, bottom_h);
    lv_obj_align(ftr, LV_ALIGN_BOTTOM_MID, 0, -margin);
    ui_set_panel_style(ftr, lv_color_white(), 18);
    lv_obj_set_style_pad_left(ftr, 22, 0);
    lv_obj_set_style_pad_right(ftr, 18, 0);

    a->status_lbl = lv_label_create(ftr);
    lv_obj_set_width(a->status_lbl, content_w - 520);
    lv_label_set_long_mode(a->status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(a->status_lbl, lv_color_make(0x64, 0x74, 0x8B), 0);
    lv_obj_align(a->status_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(a->status_lbl, "Waiting for USB camera...");

    // REINIT 按钮：放在 CHECK 左侧，用于在不影响其余 UI 的前提下复位 USB 摄像头电源。
    a->reinit_btn = lv_button_create(ftr);
    lv_obj_set_size(a->reinit_btn, 136, 46);
    lv_obj_align(a->reinit_btn, LV_ALIGN_RIGHT_MID, -336, 0);
    lv_obj_set_style_radius(a->reinit_btn, 12, 0);
    lv_obj_set_style_bg_color(a->reinit_btn, lv_color_make(0xB4, 0x53, 0x09), 0);
    lv_obj_set_style_bg_color(a->reinit_btn, lv_color_make(0xFE, 0xD7, 0xAA), LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(a->reinit_btn, 0, 0);
    lv_obj_add_event_cb(a->reinit_btn, btn_reinit_cb, LV_EVENT_CLICKED, a);
    lv_obj_t *re_lbl = lv_label_create(a->reinit_btn);
    lv_label_set_text(re_lbl, "REINIT");
    lv_obj_set_style_text_font(re_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(re_lbl, lv_color_white(), 0);
    lv_obj_center(re_lbl);

    a->check_btn = lv_button_create(ftr);
    lv_obj_set_size(a->check_btn, 136, 46);
    lv_obj_align(a->check_btn, LV_ALIGN_RIGHT_MID, -186, 0);
    lv_obj_set_style_radius(a->check_btn, 12, 0);
    lv_obj_set_style_bg_color(a->check_btn, lv_color_make(0x0F, 0x17, 0x2A), 0);
    lv_obj_set_style_bg_color(a->check_btn, lv_color_make(0xCB, 0xD5, 0xE1), LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(a->check_btn, 0, 0);
    lv_obj_add_state(a->check_btn, LV_STATE_DISABLED);
    lv_obj_add_event_cb(a->check_btn, btn_check_cb, LV_EVENT_CLICKED, a);
    lv_obj_t *chk_lbl = lv_label_create(a->check_btn);
    lv_label_set_text(chk_lbl, "CHECK");
    lv_obj_set_style_text_font(chk_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(chk_lbl, lv_color_white(), 0);
    lv_obj_center(chk_lbl);

    a->confirm_btn = lv_button_create(ftr);
    lv_obj_set_size(a->confirm_btn, 164, 46);
    lv_obj_align(a->confirm_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(a->confirm_btn, 12, 0);
    lv_obj_set_style_bg_color(a->confirm_btn, lv_color_make(0x25, 0x63, 0xEB), 0);
    lv_obj_set_style_bg_color(a->confirm_btn, lv_color_make(0xBF, 0xDB, 0xFE), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(a->confirm_btn, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(a->confirm_btn, 0, 0);
    lv_obj_add_state(a->confirm_btn, LV_STATE_DISABLED);
    lv_obj_add_event_cb(a->confirm_btn, btn_confirm_cb, LV_EVENT_CLICKED, a);
    lv_obj_t *cl = lv_label_create(a->confirm_btn);
    lv_label_set_text(cl, "START");
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cl, lv_color_white(), 0);
    lv_obj_center(cl);

    ui_update_uac_option(a);
    ui_update_info_panel(a);
    ui_update_advanced_button(a);

    otool_lvgl_port_unlock();
}
