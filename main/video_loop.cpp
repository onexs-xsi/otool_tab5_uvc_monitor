// 自动从原 main.cpp 拆分而来。仅供 main 组件内部使用。
#include "app_internal.h"

// ===================================================================
// Main decode/display loop (called after LVGL is stopped)
// ===================================================================
void drain_queue(app_t *a)
{
    uvc_host_frame_t *f = nullptr;
    while (xQueueReceive(a->frame_queue, &f, 0) == pdPASS) {
        if (f && a->stream_hdl) uvc_host_frame_return(a->stream_hdl, f);
    }
}

bool open_stream(app_t *a, int mode_idx)
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
    cfg.advanced.frame_size              = 0;
    cfg.advanced.frame_heap_caps         = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    cfg.advanced.number_of_urbs          = CAM_URB_CNT;
    cfg.advanced.urb_size                = CAM_URB_SIZE;

    ESP_LOGI(TAG,
             "Opening stream: addr=%u stream=%u fmt=%s %ux%u @%.1f frame_buffers=%d frame_size=%u urbs=%d urb_size=%u heap_caps=0x%lx",
             a->dev_addr, a->stream_idx, uvc_format_name(m->fmt), m->w, m->h, m->fps,
             cfg.advanced.number_of_frame_buffers, (unsigned)cfg.advanced.frame_size,
             cfg.advanced.number_of_urbs, (unsigned)cfg.advanced.urb_size,
             (unsigned long)cfg.advanced.frame_heap_caps);

    esp_err_t err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(5000), &a->stream_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream_open failed: %s", esp_err_to_name(err));
        a->stream_hdl = nullptr;
        return false;
    }
    err = uvc_host_stream_start(a->stream_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream_start failed: %s", esp_err_to_name(err));
        uvc_host_stream_close(a->stream_hdl);
        a->stream_hdl = nullptr;
        return false;
    }
    a->stream_active = true;
    ESP_LOGI(TAG, "Stream started: %s %ux%u @%.1f fps", uvc_format_name(m->fmt), m->w, m->h, m->fps);
    return true;
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
    uint32_t timeout_count = 0;
    uint32_t empty_frame_count = 0;

    while (a->stream_active) {
        uvc_host_frame_t *frame = nullptr;
        if (xQueueReceive(a->frame_queue, &frame, pdMS_TO_TICKS(CAM_FRAME_TIMEOUT_MS)) != pdPASS) {
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
        if (!frame || frame->data_len == 0) {
            empty_frame_count++;
            ESP_LOGW(TAG, "empty UVC frame #%u: frame=%p data_len=%u buffer_len=%u",
                     (unsigned)empty_frame_count, frame, frame ? (unsigned)frame->data_len : 0,
                     frame ? (unsigned)frame->data_buffer_len : 0);
            if (frame) uvc_host_frame_return(a->stream_hdl, frame);
            continue;
        }
        timeout_count = 0;

        if (frame->vs_format.format == UVC_VS_FORMAT_MJPEG) {
            if (!a->jpeg_dec || !a->decode_buf) {
                ESP_LOGW(TAG, "JPEG resources are not ready");
                uvc_host_frame_return(a->stream_hdl, frame);
                continue;
            }
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
            uvc_host_frame_return(a->stream_hdl, frame);

            if (err != ESP_OK) {
                ESP_LOGW(TAG, "JPEG decode err: %s", esp_err_to_name(err));
                continue;
            }

            size_t rgb_size = out_size ? out_size : ((size_t)pic_info.width * pic_info.height * 2);
            if (ppa_scale_rotate(a, a->decode_buf, rgb_size, pic_info.width, pic_info.height,
                                 PPA_SRM_COLOR_MODE_RGB565) == ESP_OK) {
                audio_record_touch_poll(a);
                draw_audio_overlay(a);
                panel_blit(a);
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
            yuyv_to_rgb565(frame->data, (uint16_t *)a->decode_buf,
                           ((size_t)src_w * src_h) / 2);
            uvc_host_frame_return(a->stream_hdl, frame);

            size_t rgb_size = (size_t)src_w * src_h * 2;
            if (ppa_scale_rotate(a, a->decode_buf, rgb_size, src_w, src_h,
                                 PPA_SRM_COLOR_MODE_RGB565) == ESP_OK) {
                audio_record_touch_poll(a);
                draw_audio_overlay(a);
                panel_blit(a);
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
            memcpy(a->decode_buf, frame->data, rgb_size);
            uvc_host_frame_return(a->stream_hdl, frame);
            if (ppa_scale_rotate(a, a->decode_buf, rgb_size, src_w, src_h,
                                 PPA_SRM_COLOR_MODE_RGB565) == ESP_OK) {
                audio_record_touch_poll(a);
                draw_audio_overlay(a);
                panel_blit(a);
            }
        } else {
            ESP_LOGW(TAG, "Unsupported frame format: %s", uvc_format_name(frame->vs_format.format));
            uvc_host_frame_return(a->stream_hdl, frame);
            continue;
        }

        a->decoded_frames++;
        uint32_t now = (uint32_t)esp_log_timestamp();
        if (now - a->fps_report_ms >= 1000) {
            a->display_fps    = a->decoded_frames;
            a->decoded_frames = 0;
            a->fps_report_ms  = now;
            ESP_LOGI(TAG, "display fps=%u", (unsigned)a->display_fps);
        }
    }
}

