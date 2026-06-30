/*
 * SPDX-FileCopyrightText: 2026 onexs-xsi
 *
 * SPDX-License-Identifier: MIT
 */

// 自动从原 main.cpp 拆分而来。仅供 main 组件内部使用。
#include "app_internal.h"

#include "esp_timer.h"

// ===================================================================
// Main decode/display loop (called after LVGL is stopped)
// ===================================================================
static void render_overlays_and_blit(app_t *a)
{
    if (a->uac_selected) {
        audio_record_touch_poll(a);
        draw_audio_overlay(a);
    }
    if (a->adv_live_enabled) {
        adv_overlay_touch_poll(a);
        draw_adv_overlay(a);
    }
    draw_factory_video_overlay(a);
    panel_blit(a);
}

void drain_queue(app_t *a)
{
    uvc_host_frame_t *f = nullptr;
    while (xQueueReceive(a->frame_queue, &f, 0) == pdPASS) {
        if (f && a->stream_hdl) uvc_host_frame_return(a->stream_hdl, f);
    }
}

static size_t next_adaptive_frame_size(size_t current)
{
    if (current < CAM_FRAME_BUF_ADAPT_FIRST) return CAM_FRAME_BUF_ADAPT_FIRST;
    if (current >= CAM_FRAME_BUF_ADAPT_MAX) return CAM_FRAME_BUF_ADAPT_MAX;

    size_t next = current + CAM_FRAME_BUF_ADAPT_STEP;
    if (next < current || next > CAM_FRAME_BUF_ADAPT_MAX) next = CAM_FRAME_BUF_ADAPT_MAX;
    return next;
}

static bool restart_stream_after_overflow(app_t *a, int mode_idx)
{
    if (!a) return false;

    a->stream_restart_requested = false;
    if (a->events && (xEventGroupGetBits(a->events) & EV_DEV_LOST)) {
        ESP_LOGW(TAG, "Skip adaptive UVC restart because device was lost");
        a->stream_active = false;
        return false;
    }

    const size_t old_size = a->uvc_frame_size_override;
    const size_t next_size = next_adaptive_frame_size(old_size);
    if (a->stream_restart_count >= CAM_OVERFLOW_RESTART_MAX || next_size == old_size) {
        ESP_LOGE(TAG,
                 "UVC frame overflow persists: frame_size=%u restart_count=%u/%u, stop stream",
                 (unsigned)old_size, (unsigned)a->stream_restart_count,
                 (unsigned)CAM_OVERFLOW_RESTART_MAX);
        a->stream_active = false;
        return false;
    }

    const uint32_t restart_no = a->stream_restart_count + 1;
    ESP_LOGW(TAG,
             "Restarting UVC stream after overflow: frame_size %s(%u) -> override(%u), attempt %u/%u",
             old_size ? "override" : "auto", (unsigned)old_size, (unsigned)next_size,
             (unsigned)restart_no, (unsigned)CAM_OVERFLOW_RESTART_MAX);

    if (a->stream_hdl) {
        esp_err_t stop_err = uvc_host_stream_stop(a->stream_hdl);
        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG, "stream_stop before adaptive restart failed: %s", esp_err_to_name(stop_err));
        }
        drain_queue(a);

        esp_err_t close_err = uvc_host_stream_close(a->stream_hdl);
        if (close_err != ESP_OK) {
            ESP_LOGW(TAG, "stream_close before adaptive restart failed: %s", esp_err_to_name(close_err));
        }
        a->stream_hdl = nullptr;
    } else {
        drain_queue(a);
    }
    a->stream_active = false;

    if (a->events && (xEventGroupGetBits(a->events) & EV_DEV_LOST)) {
        ESP_LOGW(TAG, "Skip adaptive UVC reopen because device was lost while closing old stream");
        a->stream_restart_requested = false;
        return false;
    }

    a->uvc_frame_size_override = next_size;
    a->stream_restart_count = restart_no;
    a->stream_restart_requested = false;

    if (!open_stream(a, mode_idx)) {
        ESP_LOGE(TAG, "Adaptive UVC stream reopen failed at frame_size=%u", (unsigned)next_size);
        a->stream_active = false;
        return false;
    }

    return true;
}

static bool stream_open_only(app_t *a, int mode_idx)
{
    if (mode_idx < 0 || mode_idx >= (int)a->mode_count) return false;
    if (a->stream_hdl) {
        ESP_LOGW(TAG, "stream already open, close it before opening another mode");
        return false;
    }
    const cam_mode_t *m = &a->modes[mode_idx];
    // 不做分辨率限制

    uvc_host_stream_config_t cfg = {};
    cfg.event_cb    = stream_event_cb;
    cfg.frame_cb    = frame_cb;
    cfg.user_ctx    = a;
    cfg.usb.dev_addr         = a->dev_addr;
    cfg.usb.vid              = UVC_HOST_ANY_VID;
    cfg.usb.pid              = UVC_HOST_ANY_PID;
    cfg.usb.uvc_stream_index = a->stream_idx;
    cfg.vs_format.h_res      = m->w;
    cfg.vs_format.v_res      = m->h;
    cfg.vs_format.fps        = m->fps;
    cfg.vs_format.format     = m->fmt;
    cfg.advanced.number_of_frame_buffers = CAM_FRAME_CNT;
    cfg.advanced.frame_size              = a->uvc_frame_size_override;
    cfg.advanced.frame_heap_caps         = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    cfg.advanced.number_of_urbs          = CAM_URB_CNT;
    cfg.advanced.urb_size                = CAM_URB_SIZE;

#if CAM_STREAM_OPEN_SETTLE_MS > 0
    if (a->uvc_connected_ms != 0) {
        const uint32_t now_ms = (uint32_t)esp_log_timestamp();
        const uint32_t elapsed_ms = now_ms - a->uvc_connected_ms;
        if (elapsed_ms < CAM_STREAM_OPEN_SETTLE_MS) {
            const uint32_t wait_ms = CAM_STREAM_OPEN_SETTLE_MS - elapsed_ms;
            ESP_LOGW(TAG, "Waiting %u ms for UVC device to settle before stream control", (unsigned)wait_ms);
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            if (a->events && (xEventGroupGetBits(a->events) & EV_DEV_LOST)) {
                ESP_LOGW(TAG, "Skip stream open because device was lost during settle wait");
                return false;
            }
        }
    }
#endif

#if TAB5_DIAG_STREAM_OPEN_DELAY_MS > 0
    ESP_LOGW(TAG, "Diagnostic delay before stream open: %d ms", TAB5_DIAG_STREAM_OPEN_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(TAB5_DIAG_STREAM_OPEN_DELAY_MS));
#endif

    ESP_LOGI(TAG,
             "Opening stream: addr=%u stream=%u fmt=%s %ux%u @%.1f frame_buffers=%d frame_size=%s(%u) urbs=%d urb_size=%u heap_caps=0x%lx",
             a->dev_addr, a->stream_idx, uvc_format_name(m->fmt), m->w, m->h, m->fps,
             cfg.advanced.number_of_frame_buffers,
             cfg.advanced.frame_size ? "override" : "auto", (unsigned)cfg.advanced.frame_size,
             cfg.advanced.number_of_urbs, (unsigned)cfg.advanced.urb_size,
             (unsigned long)cfg.advanced.frame_heap_caps);

    esp_err_t err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(5000), &a->stream_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream_open failed: %s", esp_err_to_name(err));
        a->stream_hdl = nullptr;
        return false;
    }

    return true;
}

static bool stream_start_only(app_t *a, const cam_mode_t *m)
{
    if (!a || !a->stream_hdl || !m) return false;

    esp_err_t err = uvc_host_stream_start(a->stream_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream_start failed: %s", esp_err_to_name(err));
        uvc_host_stream_close(a->stream_hdl);
        a->stream_hdl = nullptr;
        a->stream_active = false;
        return false;
    }
    a->stream_active = true;
    ESP_LOGI(TAG, "Stream started: %s %ux%u @%.1f fps", uvc_format_name(m->fmt), m->w, m->h, m->fps);
    return true;
}

bool open_stream(app_t *a, int mode_idx)
{
    if (!stream_open_only(a, mode_idx)) return false;

    const cam_mode_t *m = &a->modes[mode_idx];

    // 控制端点在 stream_open 后已可用；先探测并回放 pending，再启动视频流。
    // 这样 MJPEG(HW) Quality 等可能重配传感器的控制不会在 streaming 活跃后打断采集。
    cam_ctrl_probe_all(a);
    cam_ctrl_apply_pending(a);

    return stream_start_only(a, m);
}

bool probe_stream_configuration(app_t *a, int mode_idx)
{
    if (!a || mode_idx < 0 || mode_idx >= (int)a->mode_count) return false;
    if (!stream_open_only(a, mode_idx)) return false;

    ESP_LOGI(TAG, "Configuration preflight: stream opened for GET_* verification without starting video");
    cam_ctrl_probe_all(a);
    esp_err_t close_err = uvc_host_stream_close(a->stream_hdl);
    if (close_err != ESP_OK) {
        ESP_LOGW(TAG, "Configuration preflight stream close failed: %s", esp_err_to_name(close_err));
    }
    a->stream_hdl = nullptr;
    a->stream_active = false;
    drain_queue(a);
    return a->adv_caps_probed;
}

void video_loop(app_t *a)
{
    const cam_mode_t *m  = &a->modes[a->selected_mode];
    unsigned src_w = m->w, src_h = m->h;

    jpeg_decode_cfg_t dec_cfg = {};
    dec_cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    dec_cfg.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    dec_cfg.conv_std      = JPEG_YUV_RGB_CONV_STD_BT601;

    a->decoded_frames = 0;
    a->fps_report_ms  = (uint32_t)esp_log_timestamp();
    a->display_fps = 0.0f;
    uint32_t overlay_fps_ms = a->fps_report_ms;
    uint32_t overlay_frames = 0;
    uint32_t timeout_count = 0;
    uint32_t empty_frame_count = 0;
    uint64_t stat_wait_us = 0;
    uint64_t stat_decode_us = 0;
    uint64_t stat_convert_us = 0;
    uint64_t stat_ppa_us = 0;
    uint64_t stat_blit_us = 0;
    uint64_t stat_total_us = 0;

    while (a->stream_active || a->stream_restart_requested) {
        if (a->stream_restart_requested) {
            if (!restart_stream_after_overflow(a, a->selected_mode)) {
                break;
            }
            m = &a->modes[a->selected_mode];
            src_w = m->w;
            src_h = m->h;
            a->decoded_frames = 0;
            a->fps_report_ms  = (uint32_t)esp_log_timestamp();
            a->display_fps = 0.0f;
            overlay_fps_ms = a->fps_report_ms;
            overlay_frames = 0;
            timeout_count = 0;
            empty_frame_count = 0;
            stat_wait_us = 0;
            stat_decode_us = 0;
            stat_convert_us = 0;
            stat_ppa_us = 0;
            stat_blit_us = 0;
            stat_total_us = 0;
            continue;
        }

        uvc_host_frame_t *frame = nullptr;
        const int64_t frame_loop_start_us = esp_timer_get_time();
        const int64_t wait_start_us = frame_loop_start_us;
        if (xQueueReceive(a->frame_queue, &frame, pdMS_TO_TICKS(CAM_FRAME_TIMEOUT_MS)) != pdPASS) {
            if (a->stream_restart_requested) {
                continue;
            }
            if (!a->stream_active) {
                break;
            }
            timeout_count++;
            ESP_LOGW(TAG, "frame timeout #%u: no UVC frame received for %u ms stream_active=%d mode=%s %ux%u @%.1f",
                     (unsigned)timeout_count, CAM_FRAME_TIMEOUT_MS, (int)a->stream_active,
                     uvc_format_name(m->fmt), m->w, m->h, m->fps);
            if (timeout_count == CAM_NO_FRAME_DIAG_TIMEOUTS) {
                ESP_LOGW(TAG, "No complete UVC frame delivered after stream_start; check payload header compatibility, endpoint traffic, and negotiated format");
                ESP_LOGW(TAG, "If the device log shows successful UVC transfers, the host may be dropping packets before frame callback");
            }
            continue;
        }
        const int64_t got_frame_us = esp_timer_get_time();
        if (!frame || frame->data_len == 0) {
            empty_frame_count++;
            ESP_LOGW(TAG, "empty UVC frame #%u: frame=%p data_len=%u buffer_len=%u",
                     (unsigned)empty_frame_count, frame, frame ? (unsigned)frame->data_len : 0,
                     frame ? (unsigned)frame->data_buffer_len : 0);
            if (frame) uvc_host_frame_return(a->stream_hdl, frame);
            continue;
        }
        timeout_count = 0;

        bool rendered = false;
        uint64_t frame_decode_us = 0;
        uint64_t frame_convert_us = 0;
        uint64_t frame_ppa_us = 0;
        uint64_t frame_blit_us = 0;

        if (frame->vs_format.format == UVC_VS_FORMAT_MJPEG
            || frame->vs_format.format == UVC_VS_FORMAT_MJPEG_SW) {
            if (!a->jpeg_dec || !a->decode_buf) {
                ESP_LOGW(TAG, "JPEG resources are not ready");
                uvc_host_frame_return(a->stream_hdl, frame);
                continue;
            }
            const int64_t decode_start_us = esp_timer_get_time();
            size_t jpeg_len = mjpeg_valid_len(frame->data, frame->data_len);
            if (jpeg_len == 0) {
                ESP_LOGW(TAG, "Drop invalid MJPEG frame: len=%u first=%02x%02x last=%02x%02x",
                         (unsigned)frame->data_len,
                         frame->data_len > 0 ? frame->data[0] : 0,
                         frame->data_len > 1 ? frame->data[1] : 0,
                         frame->data_len > 1 ? frame->data[frame->data_len - 2] : 0,
                         frame->data_len > 0 ? frame->data[frame->data_len - 1] : 0);
                uvc_host_frame_return(a->stream_hdl, frame);
                continue;
            }

            jpeg_decode_picture_info_t pic_info = {};
            esp_err_t info_err = jpeg_decoder_get_info(frame->data, (uint32_t)jpeg_len, &pic_info);
            if (info_err != ESP_OK || pic_info.width == 0 || pic_info.height == 0 ||
                    pic_info.width > CAM_MAX_SRC_W || pic_info.height > CAM_MAX_SRC_H) {
                ESP_LOGW(TAG, "Drop MJPEG frame with bad JPEG info: err=%s %ux%u len=%u",
                         esp_err_to_name(info_err), (unsigned)pic_info.width,
                         (unsigned)pic_info.height, (unsigned)frame->data_len);
                uvc_host_frame_return(a->stream_hdl, frame);
                continue;
            }

            uint32_t out_size = 0;
            esp_err_t err = jpeg_decoder_process(a->jpeg_dec, &dec_cfg,
                                                 frame->data, (uint32_t)jpeg_len,
                                                 a->decode_buf, (uint32_t)a->decode_buf_size,
                                                 &out_size);
            frame_decode_us = (uint64_t)(esp_timer_get_time() - decode_start_us);
            uvc_host_frame_return(a->stream_hdl, frame);

            if (err != ESP_OK) {
                ESP_LOGW(TAG, "JPEG decode err: %s", esp_err_to_name(err));
                continue;
            }

            size_t rgb_size = out_size ? out_size : ((size_t)pic_info.width * pic_info.height * 2);
            const int64_t ppa_start_us = esp_timer_get_time();
            esp_err_t ppa_err = ppa_scale_rotate(a, a->decode_buf, rgb_size, pic_info.width, pic_info.height,
                                                 PPA_SRM_COLOR_MODE_RGB565);
            frame_ppa_us = (uint64_t)(esp_timer_get_time() - ppa_start_us);
            if (ppa_err == ESP_OK) {
                const int64_t blit_start_us = esp_timer_get_time();
                render_overlays_and_blit(a);
                frame_blit_us = (uint64_t)(esp_timer_get_time() - blit_start_us);
                rendered = true;
            }
        } else if (frame->vs_format.format == UVC_VS_FORMAT_YUY2) {
            size_t required = (size_t)src_w * src_h * 2;
            if (frame->data_len < required) {
                ESP_LOGW(TAG, "YUY2 frame too small: got=%u need=%u",
                         (unsigned)frame->data_len, (unsigned)required);
                uvc_host_frame_return(a->stream_hdl, frame);
                continue;
            }
            if (!a->decode_buf || a->decode_buf_size < required) {
                ESP_LOGW(TAG, "YUY2 conv buf missing: have=%u need=%u",
                         (unsigned)a->decode_buf_size, (unsigned)required);
                uvc_host_frame_return(a->stream_hdl, frame);
                continue;
            }

            // 软件 YUYV -> RGB565（PPA SRM 在本芯片版本不支持 YUYV 源）
            const int64_t convert_start_us = esp_timer_get_time();
            yuyv_to_rgb565(frame->data, (uint16_t *)a->decode_buf,
                           ((size_t)src_w * src_h) / 2);
            esp_cache_msync(a->decode_buf, required,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            frame_convert_us = (uint64_t)(esp_timer_get_time() - convert_start_us);
            uvc_host_frame_return(a->stream_hdl, frame);

            size_t rgb_size = (size_t)src_w * src_h * 2;
            const int64_t ppa_start_us = esp_timer_get_time();
            esp_err_t ppa_err = ppa_scale_rotate(a, a->decode_buf, rgb_size, src_w, src_h,
                                                 PPA_SRM_COLOR_MODE_RGB565);
            frame_ppa_us = (uint64_t)(esp_timer_get_time() - ppa_start_us);
            if (ppa_err == ESP_OK) {
                const int64_t blit_start_us = esp_timer_get_time();
                render_overlays_and_blit(a);
                frame_blit_us = (uint64_t)(esp_timer_get_time() - blit_start_us);
                rendered = true;
            }
        } else if (frame->vs_format.format == UVC_VS_FORMAT_UYVY) {
            size_t required = (size_t)src_w * src_h * 2;
            if (frame->data_len < required) {
                ESP_LOGW(TAG, "UYVY frame too small: got=%u need=%u",
                         (unsigned)frame->data_len, (unsigned)required);
                uvc_host_frame_return(a->stream_hdl, frame);
                continue;
            }
            if (!a->decode_buf || a->decode_buf_size < required) {
                ESP_LOGW(TAG, "UYVY conv buf missing: have=%u need=%u",
                         (unsigned)a->decode_buf_size, (unsigned)required);
                uvc_host_frame_return(a->stream_hdl, frame);
                continue;
            }

            // 软件 UYVY -> RGB565
            const int64_t convert_start_us = esp_timer_get_time();
            uyvy_to_rgb565(frame->data, (uint16_t *)a->decode_buf,
                           ((size_t)src_w * src_h) / 2);
            esp_cache_msync(a->decode_buf, required,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            frame_convert_us = (uint64_t)(esp_timer_get_time() - convert_start_us);
            uvc_host_frame_return(a->stream_hdl, frame);

            size_t rgb_size = (size_t)src_w * src_h * 2;
            const int64_t ppa_start_us = esp_timer_get_time();
            esp_err_t ppa_err = ppa_scale_rotate(a, a->decode_buf, rgb_size, src_w, src_h,
                                                 PPA_SRM_COLOR_MODE_RGB565);
            frame_ppa_us = (uint64_t)(esp_timer_get_time() - ppa_start_us);
            if (ppa_err == ESP_OK) {
                const int64_t blit_start_us = esp_timer_get_time();
                render_overlays_and_blit(a);
                frame_blit_us = (uint64_t)(esp_timer_get_time() - blit_start_us);
                rendered = true;
            }
        } else if (frame->vs_format.format == UVC_VS_FORMAT_GRAY8) {
            size_t required = (size_t)src_w * src_h;
            size_t rgb_size = (size_t)src_w * src_h * 2;
            if (frame->data_len < required) {
                ESP_LOGW(TAG, "GRAY8 frame too small: got=%u need=%u",
                         (unsigned)frame->data_len, (unsigned)required);
                uvc_host_frame_return(a->stream_hdl, frame);
                continue;
            }
            if (!a->decode_buf || a->decode_buf_size < rgb_size) {
                ESP_LOGW(TAG, "GRAY8 conv buf missing: have=%u need=%u",
                         (unsigned)a->decode_buf_size, (unsigned)rgb_size);
                uvc_host_frame_return(a->stream_hdl, frame);
                continue;
            }

            // 软件 GRAY8 -> RGB565
            const int64_t convert_start_us = esp_timer_get_time();
            gray8_to_rgb565(frame->data, (uint16_t *)a->decode_buf,
                            (size_t)src_w * src_h);
            esp_cache_msync(a->decode_buf, rgb_size,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            frame_convert_us = (uint64_t)(esp_timer_get_time() - convert_start_us);
            uvc_host_frame_return(a->stream_hdl, frame);

            const int64_t ppa_start_us = esp_timer_get_time();
            esp_err_t ppa_err = ppa_scale_rotate(a, a->decode_buf, rgb_size, src_w, src_h,
                                                 PPA_SRM_COLOR_MODE_RGB565);
            frame_ppa_us = (uint64_t)(esp_timer_get_time() - ppa_start_us);
            if (ppa_err == ESP_OK) {
                const int64_t blit_start_us = esp_timer_get_time();
                render_overlays_and_blit(a);
                frame_blit_us = (uint64_t)(esp_timer_get_time() - blit_start_us);
                rendered = true;
            }
        } else if (frame->vs_format.format == UVC_VS_FORMAT_RGBP) {
            // RGB565 LE (RGBP) — no color conversion needed, copy to aligned buf then PPA
            size_t rgb_size = (size_t)src_w * src_h * 2;
            if (frame->data_len < rgb_size) {
                ESP_LOGW(TAG, "RGBP frame too small: got=%u need=%u",
                         (unsigned)frame->data_len, (unsigned)rgb_size);
                uvc_host_frame_return(a->stream_hdl, frame);
                continue;
            }
            if (!a->decode_buf || a->decode_buf_size < rgb_size) {
                ESP_LOGW(TAG, "RGBP buf missing: have=%u need=%u",
                         (unsigned)a->decode_buf_size, (unsigned)rgb_size);
                uvc_host_frame_return(a->stream_hdl, frame);
                continue;
            }
            const int64_t convert_start_us = esp_timer_get_time();
            memcpy(a->decode_buf, frame->data, rgb_size);
            esp_cache_msync(a->decode_buf, rgb_size,
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            frame_convert_us = (uint64_t)(esp_timer_get_time() - convert_start_us);
            uvc_host_frame_return(a->stream_hdl, frame);
            const int64_t ppa_start_us = esp_timer_get_time();
            esp_err_t ppa_err = ppa_scale_rotate(a, a->decode_buf, rgb_size, src_w, src_h,
                                                 PPA_SRM_COLOR_MODE_RGB565);
            frame_ppa_us = (uint64_t)(esp_timer_get_time() - ppa_start_us);
            if (ppa_err == ESP_OK) {
                const int64_t blit_start_us = esp_timer_get_time();
                render_overlays_and_blit(a);
                frame_blit_us = (uint64_t)(esp_timer_get_time() - blit_start_us);
                rendered = true;
            }
        } else {
            ESP_LOGW(TAG, "Unsupported frame format: %s", uvc_format_name(frame->vs_format.format));
            uvc_host_frame_return(a->stream_hdl, frame);
            continue;
        }

        if (!rendered) {
            continue;
        }

        a->decoded_frames++;
        stat_wait_us += (uint64_t)(got_frame_us - wait_start_us);
        stat_decode_us += frame_decode_us;
        stat_convert_us += frame_convert_us;
        stat_ppa_us += frame_ppa_us;
        stat_blit_us += frame_blit_us;
        stat_total_us += (uint64_t)(esp_timer_get_time() - frame_loop_start_us);
        uint32_t now = (uint32_t)esp_log_timestamp();
        overlay_frames++;
        if (now - overlay_fps_ms >= 1000) {
            const uint32_t elapsed_ms = now - overlay_fps_ms;
            const float fps = elapsed_ms > 0 ? ((float)overlay_frames * 1000.0f / (float)elapsed_ms) : 0.0f;
            a->display_fps = fps;
            overlay_frames = 0;
            overlay_fps_ms = now;
        }
        if (now - a->fps_report_ms >= CAM_FPS_REPORT_INTERVAL_MS) {
            const uint32_t elapsed_ms = now - a->fps_report_ms;
            const uint32_t frames = a->decoded_frames;
            const float fps = elapsed_ms > 0 ? ((float)frames * 1000.0f / (float)elapsed_ms) : 0.0f;
            const float denom = frames > 0 ? (float)frames : 1.0f;
            ESP_LOGI(TAG,
                     "display stats: %.2f fps frames=%u/%ums avg_ms wait=%.2f decode=%.2f convert=%.2f ppa=%.2f blit=%.2f total=%.2f mode=%s %ux%u",
                     fps,
                     (unsigned)frames,
                     (unsigned)elapsed_ms,
                     (double)((float)stat_wait_us / denom / 1000.0f),
                     (double)((float)stat_decode_us / denom / 1000.0f),
                     (double)((float)stat_convert_us / denom / 1000.0f),
                     (double)((float)stat_ppa_us / denom / 1000.0f),
                     (double)((float)stat_blit_us / denom / 1000.0f),
                     (double)((float)stat_total_us / denom / 1000.0f),
                     uvc_format_name(m->fmt), m->w, m->h);
            a->decoded_frames = 0;
            a->fps_report_ms  = now;
            stat_wait_us = 0;
            stat_decode_us = 0;
            stat_convert_us = 0;
            stat_ppa_us = 0;
            stat_blit_us = 0;
            stat_total_us = 0;
        }
    }
}
