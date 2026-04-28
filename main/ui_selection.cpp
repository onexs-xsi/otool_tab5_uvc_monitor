// 自动从原 main.cpp 拆分而来。仅供 main 组件内部使用。
#include "app_internal.h"

// 文件内静态前向声明（事件回调互相引用）
static void check_close_cb(lv_event_t *e);
static void btn_check_cb(lv_event_t *e);
static void btn_reinit_cb(lv_event_t *e);

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
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
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
        unsigned mjpeg = 0, yuy2 = 0, rgbp = 0, other = 0;
        for (size_t i = 0; i < a->mode_count; i++) {
            switch (a->modes[i].fmt) {
            case UVC_VS_FORMAT_MJPEG: mjpeg++; break;
            case UVC_VS_FORMAT_YUY2:  yuy2++; break;
            case UVC_VS_FORMAT_RGBP:  rgbp++; break;
            default:                  other++; break;
            }
        }
        snprintf(buf, sizeof(buf), "Formats\nMJPEG %u  YUY2 %u\nRGBP %u  Other %u", mjpeg, yuy2, rgbp, other);
        lv_label_set_text(a->info_formats_lbl, buf);
    }

    if (a->info_audio_lbl) {
        const char *audio = "no UAC";
        if (a->uac_rx_available && a->uac_tx_available) audio = "mic + speaker";
        else if (a->uac_rx_available) audio = "mic only";
        else if (a->uac_tx_available) audio = "speaker only";
        snprintf(buf, sizeof(buf), "Audio\n%s\n%s", audio, a->uac_selected ? "enabled" : "disabled");
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
    lv_obj_clear_flag(a->check_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(a->check_overlay, LV_OBJ_FLAG_CLICKABLE);

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
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
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
        if (a->confirm_btn && a->mode_count > 0) lv_obj_clear_state(a->confirm_btn, LV_STATE_DISABLED);
        if (a->check_btn && a->mode_count > 0) lv_obj_clear_state(a->check_btn, LV_STATE_DISABLED);
        if (a->check_close_btn) {
            lv_obj_clear_state(a->check_close_btn, LV_STATE_DISABLED);
            lv_obj_t *lbl = lv_obj_get_child(a->check_close_btn, 0);
            if (lbl) lv_label_set_text(lbl, "DONE");
        }
        ui_update_info_panel(a);
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
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_CLICKED) return;
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

static void list_item_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= (int)a->mode_count) return;
    // 不做分辨率限制

    // 清除所有项高亮
    uint32_t cnt = lv_obj_get_child_count(a->list_obj);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *item = lv_obj_get_child(a->list_obj, i);
        ui_style_mode_item(item, false);
    }

    // 高亮当前选中项
    ui_style_mode_item(btn, true);
    a->selected_mode = idx;
    ui_update_info_panel(a);
}

static void btn_confirm_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (a->check_running) return;
    if (a->selected_mode < 0 || a->selected_mode >= (int)a->mode_count) return;
    a->uac_selected = a->uac_rx_available && a->uac_switch &&
                      lv_obj_has_state(a->uac_switch, LV_STATE_CHECKED);
    if (!mode_is_safe(&a->modes[a->selected_mode])) {
        ui_set_status(a, "Selected mode needs too much USB memory");
        return;
    }
    ESP_LOGI(TAG, "User confirmed mode %d", a->selected_mode);
    xEventGroupSetBits(a->events, EV_USER_CONFIRMED);
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
    app_request_usb_reinit(a);
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
    lv_obj_clean(a->list_obj);

    for (int i = 0; i < (int)a->mode_count; i++) {
        const cam_mode_t *m = &a->modes[i];

        char res_buf[32];
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

        snprintf(res_buf, sizeof(res_buf), "%ux%u", m->w, m->h);
        lv_obj_t *res_lbl = lv_label_create(btn);
        lv_label_set_text(res_lbl, res_buf);
        lv_obj_set_style_text_font(res_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(res_lbl, lv_color_make(0x1E, 0x29, 0x3B), 0);
        lv_obj_align(res_lbl, LV_ALIGN_LEFT_MID, 0, -10);

        snprintf(fps_buf, sizeof(fps_buf), "%.1f fps", m->fps);
        lv_obj_t *fps_lbl = lv_label_create(btn);
        lv_label_set_text(fps_lbl, fps_buf);
        lv_obj_set_style_text_color(fps_lbl, lv_color_make(0x3B, 0x82, 0xF6), 0);
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
    }
}

void ui_clear_mode_list(app_t *a)
{
    if (!a->list_obj) return;
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
    lv_obj_clear_flag(a->sel_screen, LV_OBJ_FLAG_SCROLLABLE);

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
    lv_obj_clear_flag(accent_bar, LV_OBJ_FLAG_CLICKABLE);

    a->title_lbl = lv_label_create(left_panel);
    lv_label_set_text(a->title_lbl, "OTool Tab5 UVC Monitor");
    lv_obj_set_style_text_color(a->title_lbl, lv_color_make(0x0F, 0x17, 0x2A), 0);
    lv_obj_set_style_text_font(a->title_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(a->title_lbl, LV_ALIGN_TOP_LEFT, 18, 0);

    lv_obj_t *sub_lbl = lv_label_create(left_panel);
    lv_label_set_text(sub_lbl, "Descriptor order is preserved. Pick one mode or run CHECK first.");
    lv_obj_set_style_text_color(sub_lbl, lv_color_make(0x64, 0x74, 0x8B), 0);
    lv_obj_align(sub_lbl, LV_ALIGN_TOP_LEFT, 18, 34);

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
    lv_obj_set_height(a->info_mode_lbl, 76);
    a->info_device_lbl = ui_create_info_label(right_panel, "Device\nwaiting", lv_color_make(0x47, 0x55, 0x69), nullptr);
    lv_obj_set_height(a->info_device_lbl, 70);
    a->info_formats_lbl = ui_create_info_label(right_panel, "Formats\nwaiting", lv_color_make(0x47, 0x55, 0x69), nullptr);
    lv_obj_set_height(a->info_formats_lbl, 70);
    a->info_audio_lbl = ui_create_info_label(right_panel, "Audio\nno UAC", lv_color_make(0x47, 0x55, 0x69), nullptr);
    lv_obj_set_height(a->info_audio_lbl, 64);

    lv_obj_t *uac_row = lv_obj_create(right_panel);
    lv_obj_set_width(uac_row, lv_pct(100));
    lv_obj_set_height(uac_row, 74);
    lv_obj_set_style_bg_color(uac_row, lv_color_make(0xF8, 0xFA, 0xFC), 0);
    lv_obj_set_style_bg_opa(uac_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(uac_row, 1, 0);
    lv_obj_set_style_border_color(uac_row, lv_color_make(0xE2, 0xE8, 0xF0), 0);
    lv_obj_set_style_pad_all(uac_row, 12, 0);
    lv_obj_set_style_radius(uac_row, 14, 0);
    lv_obj_clear_flag(uac_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(uac_row, LV_OBJ_FLAG_CLICKABLE);
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
    lv_obj_clear_flag(a->uac_switch, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(a->uac_switch, uac_switch_cb, LV_EVENT_VALUE_CHANGED, a);
    lv_obj_add_event_cb(a->uac_switch, uac_switch_cb, LV_EVENT_CLICKED, a);

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

    otool_lvgl_port_unlock();
}
