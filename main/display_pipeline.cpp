// 自动从原 main.cpp 拆分而来。仅供 main 组件内部使用。
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

// ===================================================================
// PPA scale+rotate: source (sw x sh) -> panel buf (PANEL_PHYS_W x PANEL_PHYS_H)
// We rotate 90deg (landscape->portrait) and scale to fit.
// ===================================================================
esp_err_t ppa_scale_rotate(app_t *a, const void *src_buf, size_t src_size,
                                  unsigned sw, unsigned sh, ppa_srm_color_mode_t in_cm)
{
    panel_dirty_reset(a);

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

    // 性能: panel_buf 在 SPIRAM 里, 整屏 1.8MB memset 大约 30~60ms.
    // 输出图像与物理屏对齐时根本没有黑边, 直接跳过清零;
    // 否则只清四条边的 strip, 避免每帧都把整个 framebuffer 写一遍.
    if (off_x == 0 && off_y == 0 &&
        out_w == PANEL_PHYS_W && out_h == PANEL_PHYS_H) {
        // no border, skip clear
    } else {
        const size_t row_bytes = (size_t)PANEL_PHYS_W * 2; // RGB565
        // top strip
        if (off_y > 0) {
            panel_cache_msync_phys_rect(a, 0, 0, PANEL_PHYS_W, off_y, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            memset(a->panel_buf, 0, (size_t)off_y * row_bytes);
            panel_cache_msync_phys_rect(a, 0, 0, PANEL_PHYS_W, off_y, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
        // bottom strip
        uint32_t bottom_y = off_y + out_h;
        if (bottom_y < PANEL_PHYS_H) {
            panel_cache_msync_phys_rect(a, 0, bottom_y, PANEL_PHYS_W, PANEL_PHYS_H - bottom_y,
                                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            memset(a->panel_buf + (size_t)bottom_y * row_bytes, 0,
                   (size_t)(PANEL_PHYS_H - bottom_y) * row_bytes);
            panel_cache_msync_phys_rect(a, 0, bottom_y, PANEL_PHYS_W, PANEL_PHYS_H - bottom_y,
                                        ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
        // left/right strips inside [off_y, bottom_y)
        if (off_x > 0 || (off_x + out_w) < PANEL_PHYS_W) {
            const size_t left_bytes  = (size_t)off_x * 2;
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
                if (left_bytes)  memset(row, 0, left_bytes);
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

    // PPA 操作写到居中区域
    // ppa_srm 要求 out buffer 是完整的 out_pic_w x out_pic_h 区域
    // 我们用整个 panel_buf 作为输出图像 (PANEL_PHYS_W x PANEL_PHYS_H),
    // 通过 out.block offset 指定起始坐标.
    //
    // 性能: 这里以前对 src_buf 做 esp_cache_msync(C2M, 1.8MB) 的写回,
    // 但 src_buf 是 JPEG 引擎通过 DMA 写入 SPIRAM 的, CPU 全程未触碰,
    // 不存在脏的 CPU 缓存行需要写回; PPA 也直接走 DMA 读 SPIRAM, 不依赖
    // CPU 缓存, 所以这次 1.8MB 缓存遍历是纯开销, 移除.
    // YUYV/RGBP 这类 CPU 生成的 RGB565 输入由调用方在转换后做 C2M.
    (void)src_size;

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

