/*
 * SPDX-FileCopyrightText: 2026 onexs-xsi
 *
 * SPDX-License-Identifier: MIT
 */

// 自动从原 main.cpp 拆分而来。仅供 main 组件内部使用。
#include "app_internal.h"

// ===================================================================
// Candidate mode management
// ===================================================================
static uint32_t fps_x10(float fps)
{
    return (fps <= 0.f) ? 0u : (uint32_t)(fps * 10.f + 0.5f);
}

const char *uvc_format_name(enum uvc_host_stream_format fmt)
{
    switch (fmt) {
    case UVC_VS_FORMAT_DEFAULT: return "DEFAULT";
    case UVC_VS_FORMAT_MJPEG:    return "MJPEG(HW)";
    case UVC_VS_FORMAT_MJPEG_SW: return "MJPEG(SW)";
    case UVC_VS_FORMAT_YUY2:    return "YUY2";
    case UVC_VS_FORMAT_UYVY:    return "UYVY";
    case UVC_VS_FORMAT_GRAY8:   return "GRAY8";
    case UVC_VS_FORMAT_RGBP:    return "RGBP";
    case UVC_VS_FORMAT_H264:    return "H264";
    case UVC_VS_FORMAT_H265:    return "H265";
    default:                    return "UNKNOWN";
    }
}

const char *uvc_stream_event_name(enum uvc_host_dev_event event)
{
    switch (event) {
    case UVC_HOST_TRANSFER_ERROR:         return "TRANSFER_ERROR";
    case UVC_HOST_DEVICE_DISCONNECTED:    return "DEVICE_DISCONNECTED";
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:  return "FRAME_BUFFER_OVERFLOW";
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW: return "FRAME_BUFFER_UNDERFLOW";
#ifdef UVC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case UVC_HOST_DEVICE_SUSPENDED:       return "DEVICE_SUSPENDED";
    case UVC_HOST_DEVICE_RESUMED:         return "DEVICE_RESUMED";
#endif
    default:                              return "UNKNOWN";
    }
}

void log_frame_info(const uvc_host_frame_info_t *fi, size_t index)
{
    ESP_LOGI(TAG,
             "  frame[%u]: fmt=%s(%d) %ux%u default_interval=%lu default_fps=%.1f interval_type=%u",
             (unsigned)index, uvc_format_name(fi->format), (int)fi->format,
             fi->h_res, fi->v_res, (unsigned long)fi->default_interval,
             IV_TO_FPS(fi->default_interval), (unsigned)fi->interval_type);

    if (fi->interval_type > 0) {
        size_t cnt = fi->interval_type;
        if (cnt > CONFIG_UVC_INTERVAL_ARRAY_SIZE) {
            ESP_LOGW(TAG,
                     "    interval list truncated: camera=%u stored=%u",
                     (unsigned)fi->interval_type, (unsigned)CONFIG_UVC_INTERVAL_ARRAY_SIZE);
            cnt = CONFIG_UVC_INTERVAL_ARRAY_SIZE;
        }
        for (size_t j = 0; j < cnt; j++) {
            ESP_LOGI(TAG, "    interval[%u]=%lu fps=%.1f",
                     (unsigned)j, (unsigned long)fi->interval[j], IV_TO_FPS(fi->interval[j]));
        }
    } else {
        ESP_LOGI(TAG,
                 "    continuous interval: min=%lu(%.1ffps) max=%lu(%.1ffps) step=%lu",
                 (unsigned long)fi->interval_min, IV_TO_FPS(fi->interval_min),
                 (unsigned long)fi->interval_max, IV_TO_FPS(fi->interval_max),
                 (unsigned long)fi->interval_step);
    }
}

bool mode_is_safe(const cam_mode_t *m)
{
    // 历史接口: 仍保留 "是否能开启" 的最终校验入口, 当前不再额外限制.
    return true;
}

bool mode_is_selectable(const cam_mode_t *m)
{
    if (!m) return false;
    // 超过 1280x720 的模式仅作为信息展示, 不允许用户选择.
    if (m->w > CAM_MAX_SELECT_W || m->h > CAM_MAX_SELECT_H) return false;
    return true;
}

static bool mode_exists(const app_t *a, unsigned w, unsigned h,
                        float fps, enum uvc_host_stream_format fmt)
{
    uint32_t f = fps_x10(fps);
    for (size_t i = 0; i < a->mode_count; i++) {
        const cam_mode_t *m = &a->modes[i];
        if (m->w == w && m->h == h && m->fmt == fmt && fps_x10(m->fps) == f)
            return true;
    }
    return false;
}

void add_mode(app_t *a, unsigned w, unsigned h,
                     float fps, enum uvc_host_stream_format fmt)
{
    if (a->mode_count >= CAM_MAX_CANDIDATES) return;
    if (fps <= 0.f) return;
    // 超过硬件解码/PPA 极限 (1920x1080) 的分辨率绝对无法处理, 仍然不列入,
    // 但 1280x720 ~ 1920x1080 之间的模式会进入列表用于展示, 由 ui_selection
    // 把 >1280x720 的项标灰、屏蔽点击.
    if (w > CAM_MAX_SRC_W || h > CAM_MAX_SRC_H) return;
    if (mode_exists(a, w, h, fps, fmt)) return;
    cam_mode_t *m = &a->modes[a->mode_count++];
    m->w = w; m->h = h; m->fps = fps; m->fmt = fmt;
}

// 选默认: 使用描述符顺序中第一个 "用户可选" 的模式; 找不到则回退到 0.
int default_mode_index(const app_t *a)
{
    for (size_t i = 0; i < a->mode_count; i++) {
        if (mode_is_selectable(&a->modes[i])) return (int)i;
    }
    return 0;
}

