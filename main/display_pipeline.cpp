/*
 * SPDX-FileCopyrightText: 2026 onexs-xsi
 *
 * SPDX-License-Identifier: MIT
 */

#include "app_internal.h"

static bool panel_clip_rect_phys(int *x, int *y, int *w, int *h)
{
    if (!x || !y || !w || !h) return false;
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > PANEL_PHYS_W) *w = PANEL_PHYS_W - *x;
    if (*y + *h > PANEL_PHYS_H) *h = PANEL_PHYS_H - *y;
    return *w > 0 && *h > 0;
}

static void panel_cache_msync_phys_rect(app_t *a, int x, int y, int w, int h, int dir_flag)
{
    if (!a || !a->panel_buf) return;
    if (!panel_clip_rect_phys(&x, &y, &w, &h)) return;

    const size_t row_bytes = (size_t)PANEL_PHYS_W * 2;
    const size_t start = (size_t)y * row_bytes + (size_t)x * 2;
    const size_t end = (size_t)(y + h - 1) * row_bytes + (size_t)(x + w) * 2;
    if (end <= start) return;

    constexpr uintptr_t cache_line = 64;
    const uintptr_t buf_begin = (uintptr_t)a->panel_buf;
    const uintptr_t buf_end = buf_begin + PANEL_BUF_SIZE;
    const uintptr_t req_begin = buf_begin + start;
    const uintptr_t req_end = buf_begin + end;
    uintptr_t sync_begin = req_begin & ~(cache_line - 1);
    uintptr_t sync_end = (req_end + cache_line - 1) & ~(cache_line - 1);
    if (sync_begin < buf_begin) sync_begin = buf_begin;
    if (sync_end > buf_end) sync_end = buf_end;
    if (sync_end <= sync_begin) return;

    esp_cache_msync((void *)sync_begin, sync_end - sync_begin, dir_flag);
}

void panel_dirty_reset(app_t *a)
{
    if (a) a->panel_dirty_count = 0;
}

static void panel_mark_dirty_phys(app_t *a, int x, int y, int w, int h)
{
    if (!a || !panel_clip_rect_phys(&x, &y, &w, &h)) return;

    if (a->panel_dirty_count < PANEL_DIRTY_RECT_MAX) {
        a->panel_dirty[a->panel_dirty_count++] = {x, y, w, h};
        return;
    }

    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;
    for (uint8_t i = 0; i < a->panel_dirty_count; i++) {
        const panel_dirty_rect_t &r = a->panel_dirty[i];
        if (r.x < x0) x0 = r.x;
        if (r.y < y0) y0 = r.y;
        if (r.x + r.w > x1) x1 = r.x + r.w;
        if (r.y + r.h > y1) y1 = r.y + r.h;
    }
    a->panel_dirty[0] = {x0, y0, x1 - x0, y1 - y0};
    a->panel_dirty_count = 1;
}

void panel_prepare_dirty_phys(app_t *a, int x, int y, int w, int h)
{
    panel_cache_msync_phys_rect(a, x, y, w, h, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    panel_mark_dirty_phys(a, x, y, w, h);
}

void panel_prepare_dirty_logic(app_t *a, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    panel_prepare_dirty_phys(a, y, PANEL_LOGIC_W - x - w, h, w);
}

static uint32_t ceil_div_u32(uint64_t n, uint32_t d)
{
    return d ? (uint32_t)((n + d - 1) / d) : 0;
}

static constexpr uint32_t PPA_SRM_SCALE_DEN = 16;
static constexpr uint32_t PPA_SRM_SCALE_UNITS_MAX = PPA_SRM_SCALE_DEN * 2;

static uint32_t ppa_clamp_scale_units(uint32_t units)
{
    if (units < 1) return 1;
    if (units > PPA_SRM_SCALE_UNITS_MAX) return PPA_SRM_SCALE_UNITS_MAX;
    return units;
}

static uint32_t ppa_scale_units_floor(float scale)
{
    if (scale < (1.0f / (float)PPA_SRM_SCALE_DEN)) {
        return 1;
    }
    return ppa_clamp_scale_units((uint32_t)(scale * (float)PPA_SRM_SCALE_DEN));
}

static uint32_t ppa_scale_units_ceil_ratio(uint32_t target_dim, uint32_t src_dim)
{
    if (target_dim == 0 || src_dim == 0) {
        return 1;
    }
    return ppa_clamp_scale_units(ceil_div_u32((uint64_t)target_dim * PPA_SRM_SCALE_DEN, src_dim));
}

static uint32_t ppa_scaled_dim(uint32_t src_dim, uint32_t scale_units)
{
    return (uint32_t)(((uint64_t)src_dim * scale_units) / PPA_SRM_SCALE_DEN);
}

static uint32_t ppa_crop_dim_for_target(uint32_t src_dim, uint32_t target_dim, uint32_t scale_units)
{
    if (src_dim == 0 || target_dim == 0 || scale_units == 0) {
        return 0;
    }

    uint32_t crop_dim = (uint32_t)((((uint64_t)target_dim + 1) * PPA_SRM_SCALE_DEN - 1) / scale_units);
    if (crop_dim > src_dim) {
        crop_dim = src_dim;
    }
    while (crop_dim > 1 && ppa_scaled_dim(crop_dim, scale_units) > target_dim) {
        crop_dim--;
    }
    return crop_dim ? crop_dim : 1;
}

esp_err_t ppa_scale_rotate(app_t *a, const void *src_buf, size_t src_size,
                           unsigned sw, unsigned sh, ppa_srm_color_mode_t in_cm)
{
    panel_dirty_reset(a);

    const uint32_t avail_phys_h = (a && a->adv_live_enabled)
                                  ? (PANEL_PHYS_H - ADV_PHY_PY_RESERVE)
                                  : PANEL_PHYS_H;
    const uint32_t target_w = PANEL_PHYS_W;
    const uint32_t target_h = avail_phys_h;

    float scale_w = (float)target_w / (float)sh;
    float scale_h = (float)target_h / (float)sw;
    float ideal_scale = (scale_w < scale_h) ? scale_w : scale_h;
    if (ideal_scale > 2.0f) ideal_scale = 2.0f;
    if (ideal_scale < 0.1f) ideal_scale = 0.1f;

    uint32_t in_block_w = sw;
    uint32_t in_block_h = sh;
    uint32_t in_off_x = 0;
    uint32_t in_off_y = 0;
    uint32_t scale_units = ppa_scale_units_floor(ideal_scale);

    const uint32_t ideal_out_w = ((uint32_t)((float)sh * ideal_scale + 0.5f)) & ~1u;
    const uint32_t ideal_out_h = ((uint32_t)((float)sw * ideal_scale + 0.5f)) & ~1u;
    const bool intended_fullscreen = (ideal_out_w == target_w && ideal_out_h == target_h);

    if (intended_fullscreen) {
        const uint32_t cover_units_w = ppa_scale_units_ceil_ratio(target_w, sh);
        const uint32_t cover_units_h = ppa_scale_units_ceil_ratio(target_h, sw);
        scale_units = (cover_units_w > cover_units_h) ? cover_units_w : cover_units_h;

        in_block_h = ppa_crop_dim_for_target(sh, target_w, scale_units);
        in_block_w = ppa_crop_dim_for_target(sw, target_h, scale_units);
        in_off_x = (sw > in_block_w) ? ((sw - in_block_w) / 2) : 0;
        in_off_y = (sh > in_block_h) ? ((sh - in_block_h) / 2) : 0;
    }

    const float scale = (float)scale_units / (float)PPA_SRM_SCALE_DEN;
    const uint32_t out_w = ppa_scaled_dim(in_block_h, scale_units);
    const uint32_t out_h = ppa_scaled_dim(in_block_w, scale_units);
    const uint32_t off_x = (target_w > out_w) ? ((target_w - out_w) / 2) : 0;
    const uint32_t off_y = (PANEL_PHYS_H - avail_phys_h) +
                           ((target_h > out_h) ? ((target_h - out_h) / 2) : 0);

    static unsigned s_log_sw = 0;
    static unsigned s_log_sh = 0;
    static uint32_t s_log_crop_w = 0;
    static uint32_t s_log_crop_h = 0;
    static uint32_t s_log_crop_x = 0;
    static uint32_t s_log_crop_y = 0;
    static uint32_t s_log_avail_h = 0;
    static uint32_t s_log_out_w = 0;
    static uint32_t s_log_out_h = 0;
    static uint32_t s_log_scale_units = 0;
    static int s_log_adv = -1;
    const int adv_enabled = (a && a->adv_live_enabled) ? 1 : 0;
    if (s_log_sw != sw || s_log_sh != sh || s_log_crop_w != in_block_w ||
        s_log_crop_h != in_block_h || s_log_crop_x != in_off_x || s_log_crop_y != in_off_y ||
        s_log_avail_h != avail_phys_h || s_log_out_w != out_w || s_log_out_h != out_h ||
        s_log_scale_units != scale_units || s_log_adv != adv_enabled) {
        const unsigned scale_milli = (unsigned)(scale * 1000.0f + 0.5f);
        ESP_LOGI(TAG,
                 "display scale: src=%ux%u crop=%ux%u+%u,%u phys=%ux%u avail_h=%u out=%ux%u off=%u,%u scale=%u.%03u q=%u/%u adv=%d",
                 sw, sh, (unsigned)in_block_w, (unsigned)in_block_h,
                 (unsigned)in_off_x, (unsigned)in_off_y,
                 PANEL_PHYS_W, PANEL_PHYS_H, (unsigned)avail_phys_h,
                 (unsigned)out_w, (unsigned)out_h, (unsigned)off_x, (unsigned)off_y,
                 scale_milli / 1000, scale_milli % 1000,
                 (unsigned)scale_units, (unsigned)PPA_SRM_SCALE_DEN, adv_enabled);
        s_log_sw = sw;
        s_log_sh = sh;
        s_log_crop_w = in_block_w;
        s_log_crop_h = in_block_h;
        s_log_crop_x = in_off_x;
        s_log_crop_y = in_off_y;
        s_log_avail_h = avail_phys_h;
        s_log_out_w = out_w;
        s_log_out_h = out_h;
        s_log_scale_units = scale_units;
        s_log_adv = adv_enabled;
    }

    if (off_x == 0 && off_y == 0 &&
        out_w == PANEL_PHYS_W && out_h == PANEL_PHYS_H) {
        // The scaled frame fully covers the physical panel.
    } else {
        const size_t row_bytes = (size_t)PANEL_PHYS_W * 2;

        if (off_y > 0) {
            panel_cache_msync_phys_rect(a, 0, 0, PANEL_PHYS_W, off_y, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            memset(a->panel_buf, 0, (size_t)off_y * row_bytes);
            panel_cache_msync_phys_rect(a, 0, 0, PANEL_PHYS_W, off_y, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }

        uint32_t bottom_y = off_y + out_h;
        if (bottom_y < PANEL_PHYS_H) {
            panel_cache_msync_phys_rect(a, 0, bottom_y, PANEL_PHYS_W, PANEL_PHYS_H - bottom_y,
                                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            memset(a->panel_buf + (size_t)bottom_y * row_bytes, 0,
                   (size_t)(PANEL_PHYS_H - bottom_y) * row_bytes);
            panel_cache_msync_phys_rect(a, 0, bottom_y, PANEL_PHYS_W, PANEL_PHYS_H - bottom_y,
                                        ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }

        if (off_x > 0 || (off_x + out_w) < PANEL_PHYS_W) {
            const size_t left_bytes = (size_t)off_x * 2;
            const size_t right_start = (size_t)(off_x + out_w) * 2;
            const size_t right_bytes = (size_t)PANEL_PHYS_W * 2 - right_start;
            if (left_bytes) {
                panel_cache_msync_phys_rect(a, 0, off_y, off_x, out_h, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            }
            if (right_bytes) {
                panel_cache_msync_phys_rect(a, off_x + out_w, off_y, PANEL_PHYS_W - off_x - out_w, out_h,
                                            ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            }
            for (uint32_t y = off_y; y < bottom_y; y++) {
                uint8_t *row = a->panel_buf + (size_t)y * row_bytes;
                if (left_bytes) memset(row, 0, left_bytes);
                if (right_bytes) memset(row + right_start, 0, right_bytes);
            }
            if (left_bytes) {
                panel_cache_msync_phys_rect(a, 0, off_y, off_x, out_h, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            }
            if (right_bytes) {
                panel_cache_msync_phys_rect(a, off_x + out_w, off_y, PANEL_PHYS_W - off_x - out_w, out_h,
                                            ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            }
        }
    }

    (void)src_size;

    ppa_srm_oper_config_t cfg = {};
    cfg.in.buffer = src_buf;
    cfg.in.pic_w = sw;
    cfg.in.pic_h = sh;
    cfg.in.block_w = in_block_w;
    cfg.in.block_h = in_block_h;
    cfg.in.block_offset_x = in_off_x;
    cfg.in.block_offset_y = in_off_y;
    cfg.in.srm_cm = in_cm;
    cfg.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    cfg.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;

    cfg.out.buffer = a->panel_buf;
    cfg.out.buffer_size = PANEL_BUF_SIZE;
    cfg.out.pic_w = PANEL_PHYS_W;
    cfg.out.pic_h = PANEL_PHYS_H;
    cfg.out.block_offset_x = off_x;
    cfg.out.block_offset_y = off_y;
    cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    cfg.out.yuv_range = PPA_COLOR_RANGE_LIMIT;
    cfg.out.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;

    cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
    cfg.scale_x = scale;
    cfg.scale_y = scale;
    cfg.rgb_swap = 0;
    cfg.byte_swap = 0;
    cfg.mode = PPA_TRANS_MODE_BLOCKING;

    return ppa_do_scale_rotate_mirror(a->ppa, &cfg);
}

esp_err_t panel_blit(app_t *a)
{
    if (a && a->panel_buf) {
        for (uint8_t i = 0; i < a->panel_dirty_count; i++) {
            const panel_dirty_rect_t &r = a->panel_dirty[i];
            panel_cache_msync_phys_rect(a, r.x, r.y, r.w, r.h, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
        panel_dirty_reset(a);
    }
    return esp_lcd_panel_draw_bitmap(a->panel, 0, 0, PANEL_PHYS_W, PANEL_PHYS_H, a->panel_buf);
}

void panel_clear(app_t *a)
{
    panel_dirty_reset(a);
    memset(a->panel_buf, 0, PANEL_BUF_SIZE);
    panel_mark_dirty_phys(a, 0, 0, PANEL_PHYS_W, PANEL_PHYS_H);
    panel_blit(a);
}

size_t mjpeg_valid_len(const uint8_t *data, size_t len)
{
    if (!data || len < 4 || data[0] != 0xFF || data[1] != 0xD8) return 0;
    for (size_t i = len - 1; i > 0; i--) {
        if (data[i - 1] == 0xFF && data[i] == 0xD9) return i + 1;
    }
    return 0;
}

static inline uint16_t yuv2rgb565(int y, int u, int v)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void yuyv_to_rgb565(const uint8_t *src, uint16_t *dst, size_t pixel_pairs)
{
    for (size_t i = 0; i < pixel_pairs; i++) {
        const int y0 = src[0];
        const int u = src[1];
        const int y1 = src[2];
        const int v = src[3];
        dst[0] = yuv2rgb565(y0, u, v);
        dst[1] = yuv2rgb565(y1, u, v);
        src += 4;
        dst += 2;
    }
}

void uyvy_to_rgb565(const uint8_t *src, uint16_t *dst, size_t pixel_pairs)
{
    for (size_t i = 0; i < pixel_pairs; i++) {
        const int u = src[0];
        const int y0 = src[1];
        const int v = src[2];
        const int y1 = src[3];
        dst[0] = yuv2rgb565(y0, u, v);
        dst[1] = yuv2rgb565(y1, u, v);
        src += 4;
        dst += 2;
    }
}

static inline uint16_t gray_to_rgb565(uint8_t gray)
{
    return (uint16_t)(((gray & 0xF8) << 8) | ((gray & 0xFC) << 3) | (gray >> 3));
}

void gray8_to_rgb565(const uint8_t *src, uint16_t *dst, size_t pixel_count)
{
    for (size_t i = 0; i < pixel_count; i++) {
        dst[i] = gray_to_rgb565(src[i]);
    }
}
