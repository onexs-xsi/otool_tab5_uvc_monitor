// 视频阶段的高级参数侧栏：LVGL 已停止，必须直接在 panel_buf 上绘制并处理触摸。
#include "app_internal.h"

static constexpr int ADV_PANEL_W = 292;
static constexpr int ADV_PANEL_X = PANEL_LOGIC_W - ADV_PANEL_W - 14;
static constexpr int ADV_PANEL_Y = 14;
static constexpr int ADV_PANEL_H = PANEL_LOGIC_H - ADV_PANEL_Y * 2;
static constexpr int ADV_ROW_X = ADV_PANEL_X + 10;
static constexpr int ADV_ROW_W = ADV_PANEL_W - 20;
static constexpr int ADV_ROW_H = 66;
static constexpr int ADV_ROW_GAP = 6;
static constexpr int ADV_ROWS_Y = ADV_PANEL_Y + 52;

static inline uint16_t adv_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline void adv_put_pixel(app_t *a, int x, int y, uint16_t color)
{
    if (!a->panel_buf || x < 0 || y < 0 || x >= PANEL_LOGIC_W || y >= PANEL_LOGIC_H) return;
    int px = y;
    int py = PANEL_LOGIC_W - 1 - x;
    if (px < 0 || py < 0 || px >= PANEL_PHYS_W || py >= PANEL_PHYS_H) return;
    uint16_t *buf = (uint16_t *)a->panel_buf;
    buf[py * PANEL_PHYS_W + px] = color;
}

static void adv_fill_rect(app_t *a, int x, int y, int w, int h, uint16_t color)
{
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            adv_put_pixel(a, x + xx, y + yy, color);
        }
    }
}

static void adv_draw_border(app_t *a, int x, int y, int w, int h, uint16_t color)
{
    for (int xx = 0; xx < w; xx++) {
        adv_put_pixel(a, x + xx, y, color);
        adv_put_pixel(a, x + xx, y + h - 1, color);
    }
    for (int yy = 0; yy < h; yy++) {
        adv_put_pixel(a, x, y + yy, color);
        adv_put_pixel(a, x + w - 1, y + yy, color);
    }
}

static const uint8_t *adv_glyph(char c)
{
    static const uint8_t blank[7] = {0,0,0,0,0,0,0};
    static const uint8_t plus[7]  = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00};
    static const uint8_t minus[7] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    static const uint8_t colon[7] = {0x00,0x04,0x04,0x00,0x04,0x04,0x00};
    static const uint8_t equal[7] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00};
    static const uint8_t d0[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E};
    static const uint8_t d1[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E};
    static const uint8_t d2[7] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F};
    static const uint8_t d3[7] = {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E};
    static const uint8_t d4[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02};
    static const uint8_t d5[7] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E};
    static const uint8_t d6[7] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E};
    static const uint8_t d7[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08};
    static const uint8_t d8[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E};
    static const uint8_t d9[7] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C};
    static const uint8_t A[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t B[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
    static const uint8_t C[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
    static const uint8_t D[7] = {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E};
    static const uint8_t E[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
    static const uint8_t F[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
    static const uint8_t G[7] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F};
    static const uint8_t H[7] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t I[7] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E};
    static const uint8_t J[7] = {0x07,0x02,0x02,0x02,0x12,0x12,0x0C};
    static const uint8_t L[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
    static const uint8_t M[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
    static const uint8_t N[7] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11};
    static const uint8_t O[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t P[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
    static const uint8_t R[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
    static const uint8_t S[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
    static const uint8_t T[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t U[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t V[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04};
    static const uint8_t W[7] = {0x11,0x11,0x11,0x15,0x15,0x15,0x0A};
    static const uint8_t Y[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04};

    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    switch (c) {
    case ' ': return blank;
    case '+': return plus;
    case '-': return minus;
    case ':': return colon;
    case '=': return equal;
    case '0': return d0; case '1': return d1; case '2': return d2; case '3': return d3; case '4': return d4;
    case '5': return d5; case '6': return d6; case '7': return d7; case '8': return d8; case '9': return d9;
    case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D; case 'E': return E;
    case 'F': return F; case 'G': return G; case 'H': return H; case 'I': return I; case 'J': return J;
    case 'L': return L; case 'M': return M; case 'N': return N; case 'O': return O; case 'P': return P;
    case 'R': return R; case 'S': return S; case 'T': return T; case 'U': return U; case 'V': return V;
    case 'W': return W; case 'Y': return Y;
    default: return blank;
    }
}

static void adv_draw_char(app_t *a, int x, int y, char c, uint16_t color, int scale)
{
    const uint8_t *g = adv_glyph(c);
    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (g[row] & (1 << (4 - col))) {
                adv_fill_rect(a, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void adv_draw_text(app_t *a, int x, int y, const char *text, uint16_t color, int scale)
{
    if (!text) return;
    int cx = x;
    while (*text) {
        adv_draw_char(a, cx, y, *text++, color, scale);
        cx += 6 * scale;
    }
}

static bool adv_is_binary(const cam_ctrl_desc_t *d)
{
    return d && d->v_min == 0 && d->v_max == 1;
}

static const char *adv_short_name(const cam_ctrl_desc_t *d)
{
    if (!d) return "?";
    if (d->entity_id == UVC_ENTITY_ID_PROCESSING_UNIT && d->selector == UVC_PU_CTRL_CONTRAST) return "CONTRAST";
    if (d->entity_id == UVC_ENTITY_ID_PROCESSING_UNIT && d->selector == UVC_PU_CTRL_SATURATION) return "SATUR";
    if (d->entity_id == UVC_ENTITY_ID_EXTENSION_UNIT && d->selector == UVC_XU_CTRL_HMIRROR) return "HMIR";
    if (d->entity_id == UVC_ENTITY_ID_EXTENSION_UNIT && d->selector == UVC_XU_CTRL_VFLIP) return "VFLIP";
    if (d->entity_id == UVC_ENTITY_ID_EXTENSION_UNIT && d->selector == UVC_XU_CTRL_SPECIAL_EFFECT) return "EFFECT";
    if (d->entity_id == UVC_ENTITY_ID_EXTENSION_UNIT && d->selector == UVC_XU_CTRL_AE_LEVEL) return "AE";
    if (d->entity_id == UVC_ENTITY_ID_EXTENSION_UNIT && d->selector == UVC_XU_CTRL_JPEG_QUALITY) return "JPG";
    if (d->entity_id == UVC_ENTITY_ID_EXTENSION_UNIT && d->selector == UVC_XU_CTRL_WB_MODE) return "WB";
    return d->name ? d->name : "?";
}

static bool adv_has_controls(const app_t *a)
{
    if (!a || !a->adv_caps_probed) return false;
    for (int i = 0; i < (int)CAM_ADV_CTRL_COUNT; i++) {
        if (a->adv_caps[i].supported) return true;
    }
    return false;
}

static int adv_supported_index_by_row(app_t *a, int row)
{
    if (!a || row < 0) return -1;
    int visible = 0;
    for (int i = 0; i < (int)CAM_ADV_CTRL_COUNT; i++) {
        if (!a->adv_caps[i].supported) continue;
        if (visible == row) return i;
        visible++;
    }
    return -1;
}

static int32_t adv_clamp(const cam_ctrl_desc_t *d, int32_t value)
{
    if (!d) return value;
    if (value < d->v_min) return d->v_min;
    if (value > d->v_max) return d->v_max;
    return value;
}

static bool adv_apply_value(app_t *a, cam_ctrl_desc_t *d, int32_t value)
{
    if (!a || !d || !d->supported) return false;
    value = adv_clamp(d, value);
    d->v_cur = value;
    esp_err_t err = ESP_FAIL;
    if (a->stream_hdl) {
        err = cam_ctrl_set_int(a->stream_hdl, d, value);
    }
    if (err == ESP_OK) {
        d->pending_apply = false;
        ESP_LOGI(TAG, "adv live: %s=%ld OK", d->name ? d->name : "?", (long)value);
        return true;
    }
    d->pending_apply = true;
    ESP_LOGW(TAG, "adv live: %s=%ld pending (%s)",
             d->name ? d->name : "?", (long)value, esp_err_to_name(err));
    return false;
}

static bool adv_read_touch_logic(app_t *a, int *logic_x, int *logic_y)
{
    if (!a->touch_handle || !logic_x || !logic_y) return false;

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

bool advanced_overlay_touch_poll(app_t *a)
{
    if (!a || !a->adv_live_enabled || !adv_has_controls(a)) {
        if (a) a->adv_touch_down = false;
        return false;
    }

    int x = 0, y = 0;
    bool pressed = adv_read_touch_logic(a, &x, &y);
    if (!pressed) {
        a->adv_touch_down = false;
        return false;
    }
    if (x < ADV_PANEL_X || x >= ADV_PANEL_X + ADV_PANEL_W ||
        y < ADV_PANEL_Y || y >= ADV_PANEL_Y + ADV_PANEL_H) {
        return false;
    }
    if (a->adv_touch_down) {
        return true;
    }
    a->adv_touch_down = true;

    const int rel_y = y - ADV_ROWS_Y;
    if (rel_y < 0) return true;
    const int slot = rel_y / (ADV_ROW_H + ADV_ROW_GAP);
    const int in_slot_y = rel_y % (ADV_ROW_H + ADV_ROW_GAP);
    if (in_slot_y >= ADV_ROW_H) return true;
    const int idx = adv_supported_index_by_row(a, slot);
    if (idx < 0) return true;

    cam_ctrl_desc_t *d = &a->adv_caps[idx];
    if (adv_is_binary(d)) {
        adv_apply_value(a, d, d->v_cur ? d->v_min : d->v_max);
        return true;
    }

    int32_t next = d->v_cur;
    if (x >= ADV_ROW_X + 96 && x < ADV_ROW_X + 120) {
        next--;
    } else if (x >= ADV_ROW_X + ADV_ROW_W - 36 && x < ADV_ROW_X + ADV_ROW_W - 12) {
        next++;
    } else {
        const int bar_x = ADV_ROW_X + 128;
        const int bar_w = ADV_ROW_W - 176;
        int pos = x - bar_x;
        if (pos < 0) pos = 0;
        if (pos >= bar_w) pos = bar_w - 1;
        const int32_t span = d->v_max - d->v_min;
        next = d->v_min + (span * pos + bar_w / 2) / (bar_w > 1 ? (bar_w - 1) : 1);
    }
    adv_apply_value(a, d, next);
    return true;
}

static void adv_draw_row(app_t *a, const cam_ctrl_desc_t *d, int row)
{
    const int x = ADV_ROW_X;
    const int y = ADV_ROWS_Y + row * (ADV_ROW_H + ADV_ROW_GAP);
    const int w = ADV_ROW_W;
    const int h = ADV_ROW_H;
    const uint16_t bg = adv_rgb565(0x07, 0x15, 0x25);
    const uint16_t border = adv_rgb565(0x1E, 0x40, 0x5A);
    const uint16_t text = adv_rgb565(0xE2, 0xE8, 0xF0);
    const uint16_t muted = adv_rgb565(0x94, 0xA3, 0xB8);
    const uint16_t accent = adv_rgb565(0x22, 0xD3, 0xEE);
    const uint16_t accent2 = adv_rgb565(0x34, 0xD3, 0x99);

    adv_fill_rect(a, x, y, w, h, bg);
    adv_draw_border(a, x, y, w, h, border);
    adv_draw_text(a, x + 10, y + 8, adv_short_name(d), text, 2);

    char value_buf[24];
    snprintf(value_buf, sizeof(value_buf), "V=%ld", (long)d->v_cur);
    adv_draw_text(a, x + 10, y + 32, value_buf, muted, 2);

    if (adv_is_binary(d)) {
        const int sx = x + w - 76;
        const int sy = y + 20;
        const uint16_t sw_bg = d->v_cur ? accent2 : adv_rgb565(0x33, 0x41, 0x55);
        adv_fill_rect(a, sx, sy, 58, 28, sw_bg);
        adv_draw_border(a, sx, sy, 58, 28, accent);
        adv_fill_rect(a, d->v_cur ? sx + 34 : sx + 4, sy + 4, 20, 20, text);
        return;
    }

    adv_fill_rect(a, x + 96, y + 24, 24, 24, border);
    adv_draw_text(a, x + 102, y + 29, "-", text, 2);
    adv_fill_rect(a, x + w - 36, y + 24, 24, 24, accent);
    adv_draw_text(a, x + w - 30, y + 29, "+", adv_rgb565(0x02, 0x08, 0x12), 2);

    const int bar_x = x + 128;
    const int bar_y = y + 34;
    const int bar_w = w - 176;
    adv_fill_rect(a, bar_x, bar_y, bar_w, 6, adv_rgb565(0x33, 0x41, 0x55));
    const int32_t span = d->v_max - d->v_min;
    int knob = 0;
    if (span > 0) {
        knob = (int)(((int64_t)(d->v_cur - d->v_min) * (bar_w - 1)) / span);
    }
    if (knob < 0) knob = 0;
    if (knob >= bar_w) knob = bar_w - 1;
    adv_fill_rect(a, bar_x, bar_y, knob + 1, 6, accent2);
    adv_fill_rect(a, bar_x + knob - 3, bar_y - 6, 7, 18, accent);
}

void draw_advanced_overlay(app_t *a)
{
    if (!a || !a->adv_live_enabled || !adv_has_controls(a) || !a->panel_buf) return;

    const uint16_t panel = adv_rgb565(0x02, 0x08, 0x12);
    const uint16_t border = adv_rgb565(0x22, 0xD3, 0xEE);
    const uint16_t title = adv_rgb565(0xE0, 0xF2, 0xFE);
    const uint16_t muted = adv_rgb565(0x94, 0xA3, 0xB8);

    adv_fill_rect(a, ADV_PANEL_X, ADV_PANEL_Y, ADV_PANEL_W, ADV_PANEL_H, panel);
    adv_draw_border(a, ADV_PANEL_X, ADV_PANEL_Y, ADV_PANEL_W, ADV_PANEL_H, border);
    adv_draw_text(a, ADV_PANEL_X + 12, ADV_PANEL_Y + 10, "ADVANCED", title, 2);
    adv_draw_text(a, ADV_PANEL_X + ADV_PANEL_W - 58, ADV_PANEL_Y + 16, a->stream_hdl ? "LIVE" : "PEND", muted, 1);

    int row = 0;
    for (int i = 0; i < (int)CAM_ADV_CTRL_COUNT; i++) {
        if (!a->adv_caps[i].supported) continue;
        if (row >= 8) break;
        adv_draw_row(a, &a->adv_caps[i], row++);
    }
}
