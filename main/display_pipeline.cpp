// 自动从原 main.cpp 拆分而来。仅供 main 组件内部使用。
#include "app_internal.h"

// ===================================================================
// PPA scale+rotate: source (sw x sh) -> panel buf (PANEL_PHYS_W x PANEL_PHYS_H)
// We rotate 90deg (landscape->portrait) and scale to fit.
// ===================================================================
esp_err_t ppa_scale_rotate(app_t *a, const void *src_buf, size_t src_size,
                                  unsigned sw, unsigned sh, ppa_srm_color_mode_t in_cm)
{
    // 旋转后图像尺寸: out_w = sh, out_h = sw (90 度)
    // 目标屏幕: PANEL_PHYS_W x PANEL_PHYS_H
    // 等比缩放: scale = min(PANEL_PHYS_W / sh, PANEL_PHYS_H / sw)
    float scale_w = (float)PANEL_PHYS_W / (float)sh;
    float scale_h = (float)PANEL_PHYS_H / (float)sw;
    float scale   = (scale_w < scale_h) ? scale_w : scale_h;
    if (scale > 2.0f) scale = 2.0f;   // PPA scale 上限
    if (scale < 0.1f) scale = 0.1f;   // PPA scale 下限

    uint32_t out_w = (uint32_t)(sh * scale + 0.5f) & ~1u; // 保证偶数
    uint32_t out_h = (uint32_t)(sw * scale + 0.5f) & ~1u;

    // 居中偏移
    uint32_t off_x = (PANEL_PHYS_W - out_w) / 2;
    uint32_t off_y = (PANEL_PHYS_H - out_h) / 2;

    // panel_buf 先清零 (黑边)
    memset(a->panel_buf, 0, PANEL_BUF_SIZE);

    // PPA 操作写到居中区域
    // ppa_srm 要求 out buffer 是完整的 out_pic_w x out_pic_h 区域
    // 我们用整个 panel_buf 作为输出图像 (PANEL_PHYS_W x PANEL_PHYS_H),
    // 通过 out.block offset 指定起始坐标
    esp_cache_msync((void *)src_buf, src_size,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    ppa_srm_oper_config_t cfg = {};
    cfg.in.buffer       = src_buf;
    cfg.in.pic_w        = sw;
    cfg.in.pic_h        = sh;
    cfg.in.block_w      = sw;
    cfg.in.block_h      = sh;
    cfg.in.block_offset_x = 0;
    cfg.in.block_offset_y = 0;
    cfg.in.srm_cm       = in_cm;
    cfg.in.yuv_range    = PPA_COLOR_RANGE_LIMIT;
    cfg.in.yuv_std      = PPA_COLOR_CONV_STD_RGB_YUV_BT601;

    cfg.out.buffer      = a->panel_buf;
    cfg.out.buffer_size = PANEL_BUF_SIZE;
    cfg.out.pic_w       = PANEL_PHYS_W;
    cfg.out.pic_h       = PANEL_PHYS_H;
    cfg.out.block_offset_x = off_x;
    cfg.out.block_offset_y = off_y;
    cfg.out.srm_cm      = PPA_SRM_COLOR_MODE_RGB565;
    cfg.out.yuv_range   = PPA_COLOR_RANGE_LIMIT;
    cfg.out.yuv_std     = PPA_COLOR_CONV_STD_RGB_YUV_BT601;

    cfg.rotation_angle  = PPA_SRM_ROTATION_ANGLE_90;
    cfg.scale_x         = scale;
    cfg.scale_y         = scale;
    cfg.rgb_swap        = 0;
    cfg.byte_swap       = 0;
    cfg.mode            = PPA_TRANS_MODE_BLOCKING;

    return ppa_do_scale_rotate_mirror(a->ppa, &cfg);
}

// ===================================================================
// Panel direct blit (整个物理屏)
// ===================================================================
esp_err_t panel_blit(app_t *a)
{
    esp_cache_msync(a->panel_buf, PANEL_BUF_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    return esp_lcd_panel_draw_bitmap(a->panel, 0, 0, PANEL_PHYS_W, PANEL_PHYS_H, a->panel_buf);
}

void panel_clear(app_t *a)
{
    memset(a->panel_buf, 0, PANEL_BUF_SIZE);
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

// YUYV (Y0 U Y1 V) -> RGB565 软件转换
// ESP32-P4 chip rev < 3.00 时 PPA SRM 不支持 YUV422 输入，必须先转 RGB565
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
        const int u  = src[1];
        const int y1 = src[2];
        const int v  = src[3];
        dst[0] = yuv2rgb565(y0, u, v);
        dst[1] = yuv2rgb565(y1, u, v);
        src += 4;
        dst += 2;
    }
}

