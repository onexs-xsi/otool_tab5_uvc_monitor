// 自动从原 main.cpp 拆分而来。仅供 main 组件内部使用。
#include "app_internal.h"

// ===================================================================
// UVC callbacks
// ===================================================================
bool frame_cb(const uvc_host_frame_t *frame, void *user_ctx)
{
    app_t *a = (app_t *)user_ctx;
    if (xQueueSendToBack(a->frame_queue, &frame, 0) == pdPASS)
        return false;  // 由应用层归还
    // 队列满时丢弃旧帧
    uvc_host_frame_t *stale = nullptr;
    if (xQueueReceive(a->frame_queue, &stale, 0) == pdPASS && stale && a->stream_hdl)
        uvc_host_frame_return(a->stream_hdl, stale);
    if (xQueueSendToBack(a->frame_queue, &frame, 0) == pdPASS)
        return false;
    return true;  // 无法入队, 立即归还
}

void stream_event_cb(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    app_t *a = (app_t *)user_ctx;
    ESP_LOGW(TAG, "UVC stream event: %s(%d)", uvc_stream_event_name(event->type), (int)event->type);
    if (event->type == UVC_HOST_TRANSFER_ERROR) {
        ESP_LOGW(TAG, "UVC transfer error: %s", esp_err_to_name(event->transfer_error.error));
    } else if (event->type == UVC_HOST_FRAME_BUFFER_OVERFLOW) {
        ESP_LOGW(TAG, "UVC frame buffer overflow: increase frame buffer size or lower resolution/fps");
    } else if (event->type == UVC_HOST_FRAME_BUFFER_UNDERFLOW) {
        ESP_LOGW(TAG, "UVC frame buffer underflow: app is holding buffers too long or too few buffers are configured");
    }

    if (event->type == UVC_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGW(TAG, "UVC device disconnected");
        a->stream_active = false;
        if (a->uac_rx_available && a->uac_addr == a->dev_addr) {
            a->audio_active = false;
            a->uac_rx_available = false;
            a->uac_tx_available = false;
            a->uac_selected = false;
        }
        uvc_host_stream_close(event->device_disconnected.stream_hdl);
        if (a->stream_hdl == event->device_disconnected.stream_hdl)
            a->stream_hdl = nullptr;
        xEventGroupSetBits(a->events, EV_DEV_LOST);
    }
}

void uvc_driver_event_cb(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    if (event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) return;
    app_t *a = (app_t *)user_ctx;

    uint8_t addr  = event->device_connected.dev_addr;
    uint8_t sidx  = event->device_connected.uvc_stream_index;
    size_t  fcnt  = event->device_connected.frame_info_num;
    ESP_LOGI(TAG, "UVC connected: addr=%u stream=%u frame_entries=%u", addr, sidx, (unsigned)fcnt);

    if (fcnt == 0) return;

    uvc_host_frame_info_t *list =
        (uvc_host_frame_info_t *)malloc(sizeof(uvc_host_frame_info_t) * fcnt);
    if (!list) { ESP_LOGE(TAG, "malloc failed"); return; }

    size_t reported_fcnt = fcnt;
    esp_err_t list_err = uvc_host_get_frame_list(addr, sidx,
            (uvc_host_frame_info_t (*)[])list, &fcnt);
    if (list_err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_host_get_frame_list(addr=%u stream=%u entries=%u) failed: %s",
                 addr, sidx, (unsigned)reported_fcnt, esp_err_to_name(list_err));
        free(list); return;
    }

    ESP_LOGI(TAG, "UVC frame list: reported=%u parsed=%u max_supported=%ux%u interval_slots=%u",
             (unsigned)reported_fcnt, (unsigned)fcnt, CAM_MAX_SRC_W, CAM_MAX_SRC_H,
             (unsigned)CONFIG_UVC_INTERVAL_ARRAY_SIZE);

    a->dev_addr    = addr;
    a->stream_idx  = sidx;
    a->mode_count  = 0;

    // 先从 VC 描述符解析 PU/XU bmControls，选择界面即可启用 ADVANCED；
    // START/CHECK 打开流后再通过 GET_* 控制请求确认精确范围。
    cam_ctrl_probe_descriptors(a, addr, sidx);

    unsigned mjpeg_entries = 0;
    unsigned yuy2_entries = 0;
    unsigned rgbp_entries = 0;
    unsigned h264_entries = 0;
    unsigned h265_entries = 0;
    unsigned other_entries = 0;

    for (size_t i = 0; i < fcnt; i++) {
        const uvc_host_frame_info_t *fi = &list[i];
        log_frame_info(fi, i);
        switch (fi->format) {
        case UVC_VS_FORMAT_MJPEG: mjpeg_entries++; break;
        case UVC_VS_FORMAT_YUY2:  yuy2_entries++; break;
        case UVC_VS_FORMAT_RGBP:  rgbp_entries++; break;
        case UVC_VS_FORMAT_H264:  h264_entries++; break;
        case UVC_VS_FORMAT_H265:  h265_entries++; break;
        default:                  other_entries++; break;
        }

        if (fi->h_res > CAM_MAX_SRC_W || fi->v_res > CAM_MAX_SRC_H) {
            ESP_LOGW(TAG, "    skipped: resolution %ux%u exceeds hw limit %ux%u",
                     fi->h_res, fi->v_res, CAM_MAX_SRC_W, CAM_MAX_SRC_H);
            continue;
        }
        // 添加默认帧率（所有格式均列出，不可解码的格式在播放时会提示）
        add_mode(a, fi->h_res, fi->v_res, IV_TO_FPS(fi->default_interval), fi->format);
        if (fi->interval_type > 0) {
            size_t cnt = fi->interval_type;
            if (cnt > CONFIG_UVC_INTERVAL_ARRAY_SIZE) cnt = CONFIG_UVC_INTERVAL_ARRAY_SIZE;
            for (size_t j = 0; j < cnt; j++)
                if (fi->interval[j]) add_mode(a, fi->h_res, fi->v_res,
                                              IV_TO_FPS(fi->interval[j]), fi->format);
        } else {
            if (fi->interval_min)  add_mode(a, fi->h_res, fi->v_res, IV_TO_FPS(fi->interval_min),  fi->format);
            if (fi->interval_max)  add_mode(a, fi->h_res, fi->v_res, IV_TO_FPS(fi->interval_max),  fi->format);
        }
    }
    free(list);

    if (a->mode_count == 0) {
        ESP_LOGW(TAG,
                 "No modes found within hw limit %ux%u: MJPEG=%u YUY2=%u RGBP=%u H264=%u H265=%u other=%u",
                 CAM_MAX_SRC_W, CAM_MAX_SRC_H,
                 mjpeg_entries, yuy2_entries, rgbp_entries, h264_entries, h265_entries, other_entries);
        return;
    }
    // 菜单顺序沿用设备描述符顺序，便于和 UVC frame list 日志一一对应。
    a->selected_mode = default_mode_index(a);
    ESP_LOGI(TAG, "%u compatible modes, default=%d (%s %ux%u @%.1f)",
             (unsigned)a->mode_count, a->selected_mode,
             uvc_format_name(a->modes[a->selected_mode].fmt),
             a->modes[a->selected_mode].w, a->modes[a->selected_mode].h,
             a->modes[a->selected_mode].fps);
    for (size_t i = 0; i < a->mode_count; i++) {
        const cam_mode_t *m = &a->modes[i];
        ESP_LOGI(TAG, "  mode[%u]: %s %ux%u @%.1f fps%s",
                 (unsigned)i, uvc_format_name(m->fmt), m->w, m->h, m->fps,
                 ((int)i == a->selected_mode) ? " [default]" : "");
    }

    xEventGroupSetBits(a->events, EV_DEV_CONNECTED);
}

void uac_driver_event_cb(uint8_t addr, uint8_t iface_num,
                                const uac_host_driver_event_t event, void *arg)
{
    app_t *a = (app_t *)arg;
    if (event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
        a->uac_addr = addr;
        a->uac_iface_num = iface_num;
        a->uac_rx_available = true;
        ESP_LOGI(TAG, "UAC RX connected: addr=%u iface=%u", addr, iface_num);
    } else if (event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
        a->uac_tx_addr = addr;
        a->uac_tx_iface_num = iface_num;
        a->uac_tx_available = true;
        a->uac_tx_start_attempted = false;
        ESP_LOGI(TAG, "UAC TX connected: addr=%u iface=%u", addr, iface_num);
    } else {
        return;
    }
    if (a->events) xEventGroupSetBits(a->events, EV_UAC_CONNECTED);
}

