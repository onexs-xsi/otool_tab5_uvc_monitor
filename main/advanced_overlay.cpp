// 视频阶段的 ADVANCED 直接叠加层：绘制到 panel_buf，避免 LVGL DSI 整屏 flush 盖住视频。
// 性能优化（A）：所有几何/文字都用「物理坐标 + 不透明 RGB565」直接写连续内存，
//   旧实现按逻辑坐标做旋转 + 每像素 alpha 混合，每像素跨 1440 字节 SPIRAM 跨步、
//   且要先读后写，导致 380x692 的面板背景填一次就 100+ ms（cache miss + read-modify-write）。
//   现在每行都是连续 uint16_t 写入，纯 SPIRAM 写带宽，不再读背景，速度提升约 10x。
//   alpha 形参保留以兼容老调用点，统一忽略；视觉上 ADV 面板从「半透明」变为「不透明」，
//   差别极小（旧实现大都用 alpha>=210，本来就接近不透明）。
#include "app_internal.h"

static inline uint16_t rgb565_adv(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ---- 物理坐标 fast-path ----
// 将逻辑矩形 (x, y, w, h) 映射到物理矩形:
//   phys_px = logic_y, phys_py = PANEL_LOGIC_W - 1 - logic_x
// 整个集合: phys px in [y, y+h), phys py in [PANEL_LOGIC_W - x - w, PANEL_LOGIC_W - x).
// panel_buf[py * PANEL_PHYS_W + px]: 内层循环走 phys_px (= logic_y) 时是连续地址.
static inline void fill_rect_phys_op(app_t *a, int phys_px, int phys_py,
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

static void fill_rect_logic_adv(app_t *a, int x, int y, int w, int h, uint16_t color, uint8_t alpha)
{
    (void)alpha; // 统一不透明绘制，alpha 仅作 API 兼容
    if (w <= 0 || h <= 0) return;
    // logic 横向(x..x+w-1) 旋转 90 度后对应 phys 纵向 py，
    // 每 phys 行(逐 phys_px = logic_y) 是连续地址，所以 phys_w = h, phys_h = w.
    fill_rect_phys_op(a, y, PANEL_LOGIC_W - x - w, /*phys_w*/ h, /*phys_h*/ w, color);
}

static void draw_rect_border_logic_adv(app_t *a, int x, int y, int w, int h, uint16_t color, uint8_t alpha)
{
    (void)alpha;
    if (w <= 0 || h <= 0) return;
    fill_rect_logic_adv(a, x,         y,         w, 1, color, 255); // top
    fill_rect_logic_adv(a, x,         y + h - 1, w, 1, color, 255); // bottom
    fill_rect_logic_adv(a, x,         y,         1, h, color, 255); // left
    fill_rect_logic_adv(a, x + w - 1, y,         1, h, color, 255); // right
}

// 旧的 put_pixel_logic_adv / blend_rgb565_adv 已不再需要。
// 触摸轮询/数值映射等仍按逻辑坐标处理，不受影响。

static void glyph5x7(char c, uint8_t rows[7])
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
    default: break;
    }
}

static void draw_char_logic_adv(app_t *a, int x, int y, char c, uint16_t color, uint8_t alpha, int scale)
{
    uint8_t rows[7];
    glyph5x7(c, rows);
    for (int yy = 0; yy < 7; yy++) {
        for (int xx = 0; xx < 5; xx++) {
            if (rows[yy] & (1 << (4 - xx))) {
                fill_rect_logic_adv(a, x + xx * scale, y + yy * scale, scale, scale, color, alpha);
            }
        }
    }
}

static void draw_text_logic_adv(app_t *a, int x, int y, const char *text, uint16_t color, uint8_t alpha, int scale)
{
    if (!text) return;
    int cx = x;
    for (const char *p = text; *p; ++p) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        draw_char_logic_adv(a, cx, y, c, color, alpha, scale);
        cx += 6 * scale;
    }
}

static bool is_binary_ctrl(const cam_ctrl_desc_t *d)
{
    return d && d->v_min == 0 && d->v_max == 1;
}

static bool row_rect_for_ctrl(const app_t *a, int ctrl_idx, int *x, int *y, int *w, int *h)
{
    if (!a || ctrl_idx < 0 || ctrl_idx >= (int)CAM_ADV_CTRL_COUNT) return false;
    int visible = 0;
    for (int i = 0; i < (int)CAM_ADV_CTRL_COUNT; i++) {
        if (!a->adv_caps[i].supported) continue;
        if (i == ctrl_idx) {
            *x = PANEL_LOGIC_W - 394 + 14;
            *y = 92 + visible * 68;
            *w = 352;
            *h = 58;
            return true;
        }
        visible++;
    }
    return false;
}

static bool read_touch_logic_adv(app_t *a, int *logic_x, int *logic_y)
{
    if (!a || !a->touch_handle || !logic_x || !logic_y) return false;

    esp_lcd_touch_point_data_t point = {};
    uint8_t cnt = 0;
    if (esp_lcd_touch_read_data(a->touch_handle) != ESP_OK) return false;
    if (esp_lcd_touch_get_data(a->touch_handle, &point, &cnt, 1) != ESP_OK || cnt == 0) return false;

    int x = PANEL_LOGIC_W - 1 - (int)point.y;
    int y = (int)point.x;
    if (x < 0 || y < 0 || x >= PANEL_LOGIC_W || y >= PANEL_LOGIC_H) return false;
    *logic_x = x;
    *logic_y = y;
    return true;
}

static void apply_adv_value(app_t *a, cam_ctrl_desc_t *d, int32_t value)
{
    if (!a || !d || !d->supported) return;
    if (value < d->v_min) value = d->v_min;
    if (value > d->v_max) value = d->v_max;
    if (value == d->v_cur) return;

    d->v_cur = value;
    esp_err_t err = ESP_FAIL;
    if (a->stream_hdl && cam_ctrl_is_live_safe(d)) {
        err = cam_ctrl_set_int(a->stream_hdl, d, d->v_cur);
    }
    d->pending_apply = (err != ESP_OK);
    ESP_LOGI(TAG, "adv overlay %s = %ld%s",
             d->name ? d->name : "?", (long)d->v_cur,
             d->pending_apply ? " (pending)" : "");
}

void adv_overlay_touch_poll(app_t *a)
{
    if (!a || !a->adv_live_enabled) {
        if (a) a->adv_touch_down = false;
        return;
    }

    int tx = 0, ty = 0;
    bool pressed = read_touch_logic_adv(a, &tx, &ty);
    if (!pressed) {
        a->adv_touch_down = false;
        return;
    }
    if (a->adv_touch_down) return;
    a->adv_touch_down = true;

    for (int i = 0; i < (int)CAM_ADV_CTRL_COUNT; i++) {
        cam_ctrl_desc_t *d = &a->adv_caps[i];
        if (!d->supported) continue;
        int x = 0, y = 0, w = 0, h = 0;
        if (!row_rect_for_ctrl(a, i, &x, &y, &w, &h)) continue;
        if (tx < x || tx >= x + w || ty < y || ty >= y + h) continue;

        int32_t value = d->v_cur;
        if (is_binary_ctrl(d)) {
            value = (d->v_cur >= d->v_max) ? d->v_min : d->v_max;
        } else {
            const int track_x = x + 18;
            const int track_w = w - 36;
            int rel = tx - track_x;
            if (rel < 0) rel = 0;
            if (rel > track_w) rel = track_w;
            const int32_t span = d->v_max - d->v_min;
            value = d->v_min + (int32_t)(((int64_t)rel * span + track_w / 2) / track_w);
        }
        apply_adv_value(a, d, value);
        return;
    }
}

void draw_adv_overlay(app_t *a)
{
    if (!a || !a->adv_live_enabled || !a->panel_buf) return;

    const uint16_t panel_bg = rgb565_adv(0xF8, 0xFA, 0xFC);
    const uint16_t row_bg = rgb565_adv(0xFF, 0xFF, 0xFF);
    const uint16_t border = rgb565_adv(0x60, 0xA5, 0xFA);
    const uint16_t text = rgb565_adv(0x0F, 0x17, 0x2A);
    const uint16_t muted = rgb565_adv(0x47, 0x55, 0x69);
    const uint16_t accent = rgb565_adv(0x25, 0x63, 0xEB);
    const uint16_t green = rgb565_adv(0x16, 0xA3, 0x4A);
    const uint16_t amber = rgb565_adv(0xD9, 0x77, 0x06);

    const int panel_x = PANEL_LOGIC_W - 394;
    const int panel_y = 14;
    const int panel_w = 380;
    const int panel_h = PANEL_LOGIC_H - 28;

    panel_prepare_dirty_logic(a, panel_x, panel_y, panel_w, panel_h);

    fill_rect_logic_adv(a, panel_x, panel_y, panel_w, panel_h, panel_bg, 232);
    draw_rect_border_logic_adv(a, panel_x, panel_y, panel_w, panel_h, border, 220);
    draw_text_logic_adv(a, panel_x + 18, panel_y + 18, "CAMERA ADVANCED", text, 245, 3);
    draw_text_logic_adv(a, panel_x + 20, panel_y + 54, "TAP ROW TO ADJUST", muted, 230, 2);

    int row_order = 0;
    for (int i = 0; i < (int)CAM_ADV_CTRL_COUNT; i++) {
        cam_ctrl_desc_t *d = &a->adv_caps[i];
        if (!d->supported) continue;
        int x = 0, y = 0, w = 0, h = 0;
        if (!row_rect_for_ctrl(a, i, &x, &y, &w, &h)) continue;
        (void)row_order++;

        fill_rect_logic_adv(a, x, y, w, h, row_bg, 224);
        draw_rect_border_logic_adv(a, x, y, w, h, d->pending_apply ? amber : rgb565_adv(0xCB, 0xD5, 0xE1), 210);

        char label[72];
        snprintf(label, sizeof(label), "%s", d->name ? d->name : "CTRL");
        draw_text_logic_adv(a, x + 12, y + 8, label, text, 245, 2);

        char val[48];
        snprintf(val, sizeof(val), "=%ld", (long)d->v_cur);
        draw_text_logic_adv(a, x + w - 92, y + 8, val, accent, 245, 2);

        if (!cam_ctrl_is_live_safe(d)) {
            draw_text_logic_adv(a, x + 12, y + 34, "NEXT START", amber, 235, 1);
        } else if (d->pending_apply) {
            draw_text_logic_adv(a, x + 12, y + 34, "PENDING", amber, 235, 1);
        } else {
            draw_text_logic_adv(a, x + 12, y + 34, "LIVE", green, 235, 1);
        }

        if (is_binary_ctrl(d)) {
            const int sw_w = 58;
            const int sw_h = 24;
            const int sx = x + w - sw_w - 14;
            const int sy = y + 28;
            uint16_t sw_col = (d->v_cur >= d->v_max) ? green : rgb565_adv(0x94, 0xA3, 0xB8);
            fill_rect_logic_adv(a, sx, sy, sw_w, sw_h, sw_col, 225);
            draw_rect_border_logic_adv(a, sx, sy, sw_w, sw_h, border, 180);
            int knob_x = (d->v_cur >= d->v_max) ? (sx + sw_w - 20) : (sx + 4);
            fill_rect_logic_adv(a, knob_x, sy + 4, 16, 16, rgb565_adv(0xFF, 0xFF, 0xFF), 255);
        } else {
            const int track_x = x + 104;
            const int track_y = y + 40;
            const int track_w = w - 122;
            fill_rect_logic_adv(a, track_x, track_y, track_w, 8, rgb565_adv(0xCB, 0xD5, 0xE1), 220);
            int pos = 0;
            const int32_t span = d->v_max - d->v_min;
            if (span > 0) pos = (int)(((int64_t)(d->v_cur - d->v_min) * track_w) / span);
            if (pos < 0) pos = 0;
            if (pos > track_w) pos = track_w;
            fill_rect_logic_adv(a, track_x, track_y, pos, 8, accent, 235);
            fill_rect_logic_adv(a, track_x + pos - 4, track_y - 5, 10, 18, accent, 245);
        }
    }
}
