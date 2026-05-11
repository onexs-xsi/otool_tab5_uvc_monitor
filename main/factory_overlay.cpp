/*
 * SPDX-FileCopyrightText: 2026 onexs-xsi
 *
 * SPDX-License-Identifier: MIT
 */

// 产线简洁模式的等待界面与视频信息叠加层。
#include "app_internal.h"

#ifndef TAB5_APP_VERSION
#define TAB5_APP_VERSION "0.0.0"
#endif

static lv_obj_t *s_factory_wait_lbl = nullptr;
static lv_obj_t *s_factory_info_lbl = nullptr;
static lv_obj_t *s_factory_version_lbl = nullptr;
static lv_timer_t *s_factory_wait_timer = nullptr;
static char s_factory_wait_base[80] = "Waiting for USB camera";
static uint8_t s_factory_wait_dot = 0;

static void factory_wait_update_label(void)
{
    if (!s_factory_wait_lbl) return;

    s_factory_wait_dot = (uint8_t)((s_factory_wait_dot % 3) + 1);
    char dots[4] = {};
    for (uint8_t i = 0; i < s_factory_wait_dot; ++i) dots[i] = '.';

    char text[96];
    snprintf(text, sizeof(text), "%s%s", s_factory_wait_base, dots);
    lv_label_set_text(s_factory_wait_lbl, text);
    lv_obj_align(s_factory_wait_lbl, LV_ALIGN_CENTER, 0, 0);
}

static void factory_wait_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    factory_wait_update_label();
}

void factory_ui_build_waiting(app_t *a)
{
#if TAB5_FACTORY_SIMPLE_MODE
    (void)a;
    if (!otool_lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "factory ui lock fail");
        return;
    }

    if (s_factory_wait_timer) {
        lv_timer_del(s_factory_wait_timer);
        s_factory_wait_timer = nullptr;
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_factory_wait_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(s_factory_wait_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_factory_wait_lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_factory_wait_lbl, "Waiting for USB camera.");
    lv_obj_align(s_factory_wait_lbl, LV_ALIGN_CENTER, 0, 0);

    s_factory_info_lbl = lv_label_create(scr);
    lv_obj_set_width(s_factory_info_lbl, PANEL_LOGIC_W - 48);
    lv_label_set_long_mode(s_factory_info_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_factory_info_lbl, lv_color_make(0xC8, 0xD1, 0xDC), 0);
    lv_label_set_text(s_factory_info_lbl, "Camera: waiting");
    lv_obj_align(s_factory_info_lbl, LV_ALIGN_BOTTOM_LEFT, 24, -22);

    s_factory_version_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(s_factory_version_lbl, lv_color_make(0xC8, 0xD1, 0xDC), 0);
    lv_label_set_text(s_factory_version_lbl, TAB5_APP_VERSION);
    lv_obj_align(s_factory_version_lbl, LV_ALIGN_BOTTOM_RIGHT, -24, -22);

    strncpy(s_factory_wait_base, "Waiting for USB camera", sizeof(s_factory_wait_base) - 1);
    s_factory_wait_base[sizeof(s_factory_wait_base) - 1] = '\0';
    s_factory_wait_dot = 0;
    factory_wait_update_label();
    s_factory_wait_timer = lv_timer_create(factory_wait_timer_cb, 450, nullptr);

    lv_obj_invalidate(scr);
    otool_lvgl_port_unlock();
#else
    (void)a;
#endif
}

void factory_ui_set_waiting_status(app_t *a, const char *status_base)
{
#if TAB5_FACTORY_SIMPLE_MODE
    (void)a;
    const char *base = status_base ? status_base : "Waiting for USB camera";
    if (!otool_lvgl_port_lock(50)) return;
    strncpy(s_factory_wait_base, base, sizeof(s_factory_wait_base) - 1);
    s_factory_wait_base[sizeof(s_factory_wait_base) - 1] = '\0';
    s_factory_wait_dot = 0;
    factory_wait_update_label();
    otool_lvgl_port_unlock();
#else
    (void)a;
    (void)status_base;
#endif
}

void factory_ui_set_waiting_info(app_t *a, const char *info)
{
#if TAB5_FACTORY_SIMPLE_MODE
    (void)a;
    if (!s_factory_info_lbl) return;
    if (!otool_lvgl_port_lock(50)) return;
    lv_label_set_text(s_factory_info_lbl, info ? info : "Camera: waiting");
    lv_obj_align(s_factory_info_lbl, LV_ALIGN_BOTTOM_LEFT, 24, -22);
    if (s_factory_version_lbl) lv_obj_align(s_factory_version_lbl, LV_ALIGN_BOTTOM_RIGHT, -24, -22);
    otool_lvgl_port_unlock();
#else
    (void)a;
    (void)info;
#endif
}

void factory_ui_set_mode_info(app_t *a)
{
#if TAB5_FACTORY_SIMPLE_MODE
    if (!a) return;
    char info[144];
    if (a->dev_addr != 0 && a->selected_mode >= 0 && a->selected_mode < (int)a->mode_count) {
        const cam_mode_t *m = &a->modes[a->selected_mode];
        snprintf(info, sizeof(info), "Camera addr %u stream %u | %s %ux%u",
                 a->dev_addr, a->stream_idx, uvc_format_name(m->fmt), m->w, m->h);
    } else if (a->dev_addr != 0) {
        snprintf(info, sizeof(info), "Camera addr %u stream %u", a->dev_addr, a->stream_idx);
    } else {
        snprintf(info, sizeof(info), "Camera: waiting");
    }
    factory_ui_set_waiting_info(a, info);
#else
    (void)a;
#endif
}

static inline uint16_t rgb565_factory(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline void fill_rect_phys_factory(app_t *a, int phys_px, int phys_py,
                                          int phys_w, int phys_h, uint16_t color)
{
    if (!a || !a->panel_buf) return;
    if (phys_px < 0) { phys_w += phys_px; phys_px = 0; }
    if (phys_py < 0) { phys_h += phys_py; phys_py = 0; }
    if (phys_px + phys_w > PANEL_PHYS_W) phys_w = PANEL_PHYS_W - phys_px;
    if (phys_py + phys_h > PANEL_PHYS_H) phys_h = PANEL_PHYS_H - phys_py;
    if (phys_w <= 0 || phys_h <= 0) return;

    uint16_t *buf = (uint16_t *)a->panel_buf;
    for (int row = 0; row < phys_h; row++) {
        uint16_t *p = &buf[(phys_py + row) * PANEL_PHYS_W + phys_px];
        for (int i = 0; i < phys_w; i++) p[i] = color;
    }
}

static void fill_rect_logic_factory(app_t *a, int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    fill_rect_phys_factory(a, y, PANEL_LOGIC_W - x - w, h, w, color);
}

static void glyph5x7_factory(char c, uint8_t rows[7])
{
    memset(rows, 0, 7);
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    switch (c) {
    case 'A': { uint8_t r[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}; memcpy(rows,r,7); break; }
    case 'B': { uint8_t r[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}; memcpy(rows,r,7); break; }
    case 'C': { uint8_t r[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}; memcpy(rows,r,7); break; }
    case 'D': { uint8_t r[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}; memcpy(rows,r,7); break; }
    case 'E': { uint8_t r[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}; memcpy(rows,r,7); break; }
    case 'F': { uint8_t r[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}; memcpy(rows,r,7); break; }
    case 'G': { uint8_t r[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}; memcpy(rows,r,7); break; }
    case 'H': { uint8_t r[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}; memcpy(rows,r,7); break; }
    case 'I': { uint8_t r[7] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}; memcpy(rows,r,7); break; }
    case 'J': { uint8_t r[7] = {0x01,0x01,0x01,0x01,0x11,0x11,0x0E}; memcpy(rows,r,7); break; }
    case 'K': { uint8_t r[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11}; memcpy(rows,r,7); break; }
    case 'L': { uint8_t r[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}; memcpy(rows,r,7); break; }
    case 'M': { uint8_t r[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}; memcpy(rows,r,7); break; }
    case 'N': { uint8_t r[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11}; memcpy(rows,r,7); break; }
    case 'O': { uint8_t r[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}; memcpy(rows,r,7); break; }
    case 'P': { uint8_t r[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}; memcpy(rows,r,7); break; }
    case 'Q': { uint8_t r[7] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}; memcpy(rows,r,7); break; }
    case 'R': { uint8_t r[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}; memcpy(rows,r,7); break; }
    case 'S': { uint8_t r[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}; memcpy(rows,r,7); break; }
    case 'T': { uint8_t r[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}; memcpy(rows,r,7); break; }
    case 'U': { uint8_t r[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}; memcpy(rows,r,7); break; }
    case 'V': { uint8_t r[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}; memcpy(rows,r,7); break; }
    case 'W': { uint8_t r[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}; memcpy(rows,r,7); break; }
    case 'X': { uint8_t r[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}; memcpy(rows,r,7); break; }
    case 'Y': { uint8_t r[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}; memcpy(rows,r,7); break; }
    case 'Z': { uint8_t r[7] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}; memcpy(rows,r,7); break; }
    case '0': { uint8_t r[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}; memcpy(rows,r,7); break; }
    case '1': { uint8_t r[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}; memcpy(rows,r,7); break; }
    case '2': { uint8_t r[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}; memcpy(rows,r,7); break; }
    case '3': { uint8_t r[7] = {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E}; memcpy(rows,r,7); break; }
    case '4': { uint8_t r[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}; memcpy(rows,r,7); break; }
    case '5': { uint8_t r[7] = {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E}; memcpy(rows,r,7); break; }
    case '6': { uint8_t r[7] = {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E}; memcpy(rows,r,7); break; }
    case '7': { uint8_t r[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}; memcpy(rows,r,7); break; }
    case '8': { uint8_t r[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}; memcpy(rows,r,7); break; }
    case '9': { uint8_t r[7] = {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}; memcpy(rows,r,7); break; }
    case '-': { uint8_t r[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}; memcpy(rows,r,7); break; }
    case '+': { uint8_t r[7] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}; memcpy(rows,r,7); break; }
    case ':': { uint8_t r[7] = {0x00,0x04,0x04,0x00,0x04,0x04,0x00}; memcpy(rows,r,7); break; }
    case '=': { uint8_t r[7] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}; memcpy(rows,r,7); break; }
    case '/': { uint8_t r[7] = {0x01,0x01,0x02,0x04,0x08,0x10,0x10}; memcpy(rows,r,7); break; }
    case '.': { uint8_t r[7] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}; memcpy(rows,r,7); break; }
    case '|': { uint8_t r[7] = {0x04,0x04,0x04,0x04,0x04,0x04,0x04}; memcpy(rows,r,7); break; }
    default: break;
    }
}

static void draw_char_logic_factory(app_t *a, int x, int y, char c, uint16_t color, int scale)
{
    uint8_t rows[7];
    glyph5x7_factory(c, rows);
    for (int yy = 0; yy < 7; yy++) {
        for (int xx = 0; xx < 5; xx++) {
            if (rows[yy] & (1 << (4 - xx))) {
                fill_rect_logic_factory(a, x + xx * scale, y + yy * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text_logic_factory(app_t *a, int x, int y, const char *text, uint16_t color, int scale)
{
    if (!text) return;
    int cx = x;
    for (const char *p = text; *p; ++p) {
        draw_char_logic_factory(a, cx, y, *p, color, scale);
        cx += 6 * scale;
    }
}

static int text_width_logic_factory(const char *text, int scale)
{
    return text ? (int)strlen(text) * 6 * scale : 0;
}

static const char *factory_short_format_name(enum uvc_host_stream_format fmt)
{
    switch (fmt) {
    case UVC_VS_FORMAT_MJPEG: return "MJPEG";
    case UVC_VS_FORMAT_MJPEG_SW: return "MJPEGSW";
    case UVC_VS_FORMAT_YUY2: return "YUY2";
    case UVC_VS_FORMAT_UYVY: return "UYVY";
    case UVC_VS_FORMAT_RGBP: return "RGBP";
    default: return "VIDEO";
    }
}

void draw_factory_video_overlay(app_t *a)
{
#if TAB5_FACTORY_SIMPLE_MODE
    if (!a || !a->factory_simple_mode_active || !a->panel_buf) return;

    const int margin = 14;
    const int scale = 2;
    const int line_h = 7 * scale;
    const int line_gap = 6;
    const uint16_t bg = rgb565_factory(0x00, 0x00, 0x00);
    const uint16_t fg = rgb565_factory(0xFF, 0xFF, 0xFF);
    const uint16_t info_fg = rgb565_factory(0xD7, 0xE2, 0xEF);
    const uint16_t accent = rgb565_factory(0x38, 0xB4, 0xF8);

    char info0[72];
    char info1[72];
    if (a->selected_mode >= 0 && a->selected_mode < (int)a->mode_count) {
        const cam_mode_t *m = &a->modes[a->selected_mode];
        snprintf(info0, sizeof(info0), "CAM ADDR %u STREAM %u", a->dev_addr, a->stream_idx);
        snprintf(info1, sizeof(info1), "%s %ux%u FPS %u",
                 factory_short_format_name(m->fmt), m->w, m->h, (unsigned)a->display_fps);
    } else {
        snprintf(info0, sizeof(info0), "CAM ADDR %u STREAM %u", a->dev_addr, a->stream_idx);
        snprintf(info1, sizeof(info1), "FPS %u", (unsigned)a->display_fps);
    }

    const int info_w0 = text_width_logic_factory(info0, scale);
    const int info_w1 = text_width_logic_factory(info1, scale);
    const int info_w = ((info_w0 > info_w1) ? info_w0 : info_w1) + 18;
    const int info_h = line_h * 2 + line_gap + 14;
    const int info_x = margin;
    const int info_y = PANEL_LOGIC_H - margin - info_h;
    panel_prepare_dirty_logic(a, info_x, info_y, info_w, info_h);
    fill_rect_logic_factory(a, info_x, info_y, info_w, info_h, bg);
    draw_text_logic_factory(a, info_x + 8, info_y + 7, info0, info_fg, scale);
    draw_text_logic_factory(a, info_x + 8, info_y + 7 + line_h + line_gap, info1, accent, scale);

    const char *ver = TAB5_APP_VERSION;
    const int ver_w = text_width_logic_factory(ver, scale) + 18;
    const int ver_h = line_h + 14;
    const int ver_x = PANEL_LOGIC_W - margin - ver_w;
    const int ver_y = PANEL_LOGIC_H - margin - ver_h;
    panel_prepare_dirty_logic(a, ver_x, ver_y, ver_w, ver_h);
    fill_rect_logic_factory(a, ver_x, ver_y, ver_w, ver_h, bg);
    draw_text_logic_factory(a, ver_x + 8, ver_y + 7, ver, fg, scale);
#else
    (void)a;
#endif
}
