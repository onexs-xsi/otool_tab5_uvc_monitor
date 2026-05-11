/*
 * SPDX-FileCopyrightText: 2026 onexs-xsi
 *
 * SPDX-License-Identifier: MIT
 */

// 自动从原 main.cpp 拆分而来。仅供 main 组件内部使用。
#include "app_internal.h"

// uvc_host_usb_ctrl 的公开声明位于 esp_private/uvc_control.h，但该头文件依赖
// 组件内部的 usb_types_uvc.h（位于 private_include），未对外暴露。
// 为了避免修改 usb_host_uvc 组件的 CMakeLists，这里直接前向声明所需 API。
extern "C" esp_err_t uvc_host_usb_ctrl(uvc_host_stream_hdl_t stream_hdl,
                                       uint8_t bmRequestType,
                                       uint8_t bRequest,
                                       uint16_t wValue,
                                       uint16_t wIndex,
                                       uint16_t wLength,
                                       uint8_t *data);

// ===================================================================
// USB host background task
// ===================================================================
void usb_lib_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

// ===================================================================
// UVC VideoControl 实体（PU/XU）控制传输
// ===================================================================
//
// VC 接口号：Cam 端固定为 0（ITF_NUM_VIDEO_CONTROL=0）。如未来 Cam 端
// 改变了接口排布，需要在此处同步更新。
#define UVC_VC_INTERFACE_NUM   0

// kAdvCtrlTable 描述 Tab5 端期望尝试探测的所有控件，与 Cam 端 kUvcCtrlMap
// 保持一一对应（顺序也保持一致以便日志对齐）。
static const cam_ctrl_desc_t kAdvCtrlTable[CAM_ADV_CTRL_COUNT] = {
    { UVC_ENTITY_ID_PROCESSING_UNIT, UVC_PU_CTRL_CONTRAST,       2, false, -2,  2,  0,  0, false, "Contrast"      },
    { UVC_ENTITY_ID_PROCESSING_UNIT, UVC_PU_CTRL_SATURATION,     2, false, -2,  2,  0,  0, false, "Saturation"    },
    { UVC_ENTITY_ID_EXTENSION_UNIT,  UVC_XU_CTRL_HMIRROR,        4, false,  0,  1,  0,  0, false, "HMirror"       },
    { UVC_ENTITY_ID_EXTENSION_UNIT,  UVC_XU_CTRL_VFLIP,          4, false,  0,  1,  0,  0, false, "VFlip"         },
    { UVC_ENTITY_ID_EXTENSION_UNIT,  UVC_XU_CTRL_SPECIAL_EFFECT, 4, false,  0,  6,  0,  0, false, "Effect"        },
    { UVC_ENTITY_ID_EXTENSION_UNIT,  UVC_XU_CTRL_AE_LEVEL,       4, false, -5,  5,  0,  0, false, "AE Level"      },
    { UVC_ENTITY_ID_EXTENSION_UNIT,  UVC_XU_CTRL_MJPEG_HW_QUALITY,4, false,  1, 63, 17, 17, false, "MJPEG(HW) Quality" },
    { UVC_ENTITY_ID_EXTENSION_UNIT,  UVC_XU_CTRL_WB_MODE,        4, false,  0,  4,  0,  0, false, "WB Mode"       },
    { UVC_ENTITY_ID_EXTENSION_UNIT,  UVC_XU_CTRL_MJPEG_SW_QUALITY,4, false,  1,100, 60, 60, false, "MJPEG(SW) Quality" },
};

static int cam_ctrl_find_table_index(uint8_t entity_id, uint8_t selector)
{
    for (size_t i = 0; i < CAM_ADV_CTRL_COUNT; i++) {
        if (kAdvCtrlTable[i].entity_id == entity_id && kAdvCtrlTable[i].selector == selector) {
            return (int)i;
        }
    }
    return -1;
}

static inline uint16_t cam_ctrl_wvalue(uint8_t selector)
{
    return (uint16_t)((uint16_t)selector << 8);
}

static inline uint16_t cam_ctrl_windex(uint8_t entity_id)
{
    return (uint16_t)(((uint16_t)entity_id << 8) | UVC_VC_INTERFACE_NUM);
}

// 将 wire 字节流 (LE) 解码成 int32_t（按 value_size 1/2/4 字节有符号扩展）
static int32_t cam_ctrl_decode_int(const uint8_t *buf, uint8_t size)
{
    if (size == 1) {
        return (int32_t)(int8_t)buf[0];
    } else if (size == 2) {
        int16_t v = (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
        return (int32_t)v;
    } else if (size == 4) {
        return (int32_t)((uint32_t)buf[0] |
                         ((uint32_t)buf[1] << 8) |
                         ((uint32_t)buf[2] << 16) |
                         ((uint32_t)buf[3] << 24));
    }
    return 0;
}

static void cam_ctrl_encode_int(uint8_t *buf, uint8_t size, int32_t value)
{
    uint32_t u = (uint32_t)value;
    if (size >= 1) buf[0] = (uint8_t)(u & 0xFF);
    if (size >= 2) buf[1] = (uint8_t)((u >> 8) & 0xFF);
    if (size >= 3) buf[2] = (uint8_t)((u >> 16) & 0xFF);
    if (size >= 4) buf[3] = (uint8_t)((u >> 24) & 0xFF);
}

// 单个 GET_* 请求；返回 ESP_OK 表示传输成功并填充了 buf。
static esp_err_t cam_ctrl_get(uvc_host_stream_hdl_t stream_hdl,
                              uint8_t bRequest,
                              uint8_t entity_id, uint8_t selector,
                              uint8_t *buf, uint16_t len)
{
    return uvc_host_usb_ctrl(stream_hdl,
                             0xA1,                       // bmRequestType: device-to-host, class, interface
                             bRequest,
                             cam_ctrl_wvalue(selector),
                             cam_ctrl_windex(entity_id),
                             len, buf);
}

esp_err_t cam_ctrl_probe_one(uvc_host_stream_hdl_t stream_hdl, cam_ctrl_desc_t *d)
{
    if (!stream_hdl || !d) return ESP_ERR_INVALID_ARG;

    uint8_t info = 0;
    esp_err_t err = cam_ctrl_get(stream_hdl, UVC_REQ_GET_INFO, d->entity_id, d->selector, &info, 1);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "ctrl probe %s: GET_INFO failed (%s) -> unsupported",
                 d->name ? d->name : "?", esp_err_to_name(err));
        d->supported = false;
        return err;
    }
    // info bit0 = supports GET, bit1 = supports SET
    if ((info & 0x03) != 0x03) {
        ESP_LOGI(TAG, "ctrl probe %s: info=0x%02x lacks GET/SET -> unsupported",
                 d->name ? d->name : "?", info);
        d->supported = false;
        return ESP_OK;
    }

    uint8_t buf[4] = {0};
    err = cam_ctrl_get(stream_hdl, UVC_REQ_GET_MIN, d->entity_id, d->selector, buf, d->value_size);
    if (err != ESP_OK) { d->supported = false; return err; }
    d->v_min = cam_ctrl_decode_int(buf, d->value_size);

    err = cam_ctrl_get(stream_hdl, UVC_REQ_GET_MAX, d->entity_id, d->selector, buf, d->value_size);
    if (err != ESP_OK) { d->supported = false; return err; }
    d->v_max = cam_ctrl_decode_int(buf, d->value_size);

    err = cam_ctrl_get(stream_hdl, UVC_REQ_GET_DEF, d->entity_id, d->selector, buf, d->value_size);
    if (err == ESP_OK) {
        d->v_def = cam_ctrl_decode_int(buf, d->value_size);
    } else {
        // 部分实现可能不支持 GET_DEF；用 v_min 兜底。
        d->v_def = d->v_min;
    }

    err = cam_ctrl_get(stream_hdl, UVC_REQ_GET_CUR, d->entity_id, d->selector, buf, d->value_size);
    if (err == ESP_OK) {
        d->v_cur = cam_ctrl_decode_int(buf, d->value_size);
    } else {
        d->v_cur = d->v_def;
    }
    d->supported = true;
    ESP_LOGI(TAG, "ctrl probe %s: min=%ld max=%ld def=%ld cur=%ld OK",
             d->name ? d->name : "?",
             (long)d->v_min, (long)d->v_max, (long)d->v_def, (long)d->v_cur);
    return ESP_OK;
}

void cam_ctrl_init_table(app_t *a)
{
    if (!a) return;
    for (size_t i = 0; i < CAM_ADV_CTRL_COUNT; i++) {
        a->adv_caps[i] = kAdvCtrlTable[i];
    }
    a->adv_caps_probed = false;
}

void cam_ctrl_probe_descriptors(app_t *a, uint8_t dev_addr, uint8_t uvc_stream_index)
{
    if (!a) return;

    cam_ctrl_init_table(a);

    size_t ctrl_count = 0;
    esp_err_t err = uvc_host_get_ctrl_list(dev_addr, uvc_stream_index, NULL, &ctrl_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adv desc: control descriptor parse failed: %s", esp_err_to_name(err));
        return;
    }
    if (ctrl_count == 0) {
        a->adv_caps_probed = true;
        ESP_LOGI(TAG, "adv desc: no PU/XU controls advertised");
        return;
    }

    uvc_host_ctrl_info_t *ctrls = (uvc_host_ctrl_info_t *)calloc(ctrl_count, sizeof(uvc_host_ctrl_info_t));
    if (!ctrls) {
        ESP_LOGE(TAG, "adv desc: malloc failed for %u controls", (unsigned)ctrl_count);
        return;
    }

    size_t parsed_count = ctrl_count;
    err = uvc_host_get_ctrl_list(dev_addr, uvc_stream_index,
                                 (uvc_host_ctrl_info_t (*)[])ctrls, &parsed_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "adv desc: get control list failed: %s", esp_err_to_name(err));
        free(ctrls);
        return;
    }

    int supported = 0;
    for (size_t i = 0; i < parsed_count && i < ctrl_count; i++) {
        const uvc_host_ctrl_info_t *c = &ctrls[i];
        const int idx = cam_ctrl_find_table_index(c->entity_id, c->selector);
        ESP_LOGI(TAG,
                 "adv desc: entity=%u subtype=0x%02x selector=0x%02x bm[%u].bit%u%s",
                 (unsigned)c->entity_id,
                 (unsigned)c->entity_subtype,
                 (unsigned)c->selector,
                 (unsigned)c->control_byte,
                 (unsigned)c->control_bit,
                 (idx >= 0) ? " matched" : " ignored");
        if (idx < 0) {
            continue;
        }
        cam_ctrl_desc_t *d = &a->adv_caps[idx];
        if (!d->supported) {
            d->supported = true;
            d->pending_apply = false;
            d->v_cur = d->v_def;
            supported++;
        }
    }
    a->adv_caps_probed = (supported == 0);
    ESP_LOGI(TAG,
             "adv desc: %d/%d candidate controls advertised by descriptor; CONFIGURATION waits for stream GET_* verification",
             supported, (int)CAM_ADV_CTRL_COUNT);
    free(ctrls);
}

void cam_ctrl_probe_all(app_t *a)
{
    if (!a || !a->stream_hdl) return;
    int ok_cnt = 0;
    for (size_t i = 0; i < CAM_ADV_CTRL_COUNT; i++) {
        int32_t prev_cur = a->adv_caps[i].v_cur;
        bool had_pending_apply = a->adv_caps[i].pending_apply;
        cam_ctrl_probe_one(a->stream_hdl, &a->adv_caps[i]);
        if (a->adv_caps[i].supported) {
            ok_cnt++;
            if (had_pending_apply) {
                // Only preserve an explicit user override. Descriptor probing
                // uses static fallback values, so keeping that placeholder here
                // would hide the real GET_CUR value reported by the device.
                if (prev_cur < a->adv_caps[i].v_min) prev_cur = a->adv_caps[i].v_min;
                if (prev_cur > a->adv_caps[i].v_max) prev_cur = a->adv_caps[i].v_max;
                a->adv_caps[i].v_cur = prev_cur;
                a->adv_caps[i].pending_apply = true;
            } else {
                a->adv_caps[i].pending_apply = false;
            }
        }
    }
    a->adv_caps_probed = true;
    ESP_LOGI(TAG, "ctrl verify: %d/%d controls responded to GET_*", ok_cnt, (int)CAM_ADV_CTRL_COUNT);

    // 打印传感器实际支持的高级控件列表
    {
        char adv_buf[256];
        int pos = 0;
        int cnt = 0;
        for (size_t i = 0; i < CAM_ADV_CTRL_COUNT; i++) {
            if (!a->adv_caps[i].supported) continue;
            if (cnt > 0 && pos < (int)sizeof(adv_buf) - 2) {
                adv_buf[pos++] = ',';
                adv_buf[pos++] = ' ';
            }
            const char *name = a->adv_caps[i].name ? a->adv_caps[i].name : "?";
            size_t nlen = strlen(name);
            if (pos + (int)nlen < (int)sizeof(adv_buf) - 1) {
                memcpy(adv_buf + pos, name, nlen);
                pos += (int)nlen;
            }
            cnt++;
        }
        adv_buf[pos] = '\0';
        if (cnt > 0) {
            ESP_LOGI(TAG, "ADV verified (%d): %s", cnt, adv_buf);
        } else {
            ESP_LOGI(TAG, "ADV verified: none");
        }
    }
}

esp_err_t cam_ctrl_set_int(uvc_host_stream_hdl_t stream_hdl,
                           const cam_ctrl_desc_t *d, int32_t value)
{
    if (!stream_hdl || !d || !d->supported) return ESP_ERR_INVALID_ARG;
    if (d->value_size != 2 && d->value_size != 4) return ESP_ERR_INVALID_SIZE;
    uint8_t buf[4] = {0};
    cam_ctrl_encode_int(buf, d->value_size, value);
    esp_err_t err = uvc_host_usb_ctrl(stream_hdl,
                                      0x21,                  // host-to-device, class, interface
                                      UVC_REQ_SET_CUR,
                                      cam_ctrl_wvalue(d->selector),
                                      cam_ctrl_windex(d->entity_id),
                                      d->value_size, buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SET_CUR %s value=%ld failed: %s",
                 d->name ? d->name : "?", (long)value, esp_err_to_name(err));
        return err;
    }

    uint8_t rb[4] = {0};
    err = cam_ctrl_get(stream_hdl, UVC_REQ_GET_CUR, d->entity_id, d->selector, rb, d->value_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET_CUR verify %s value=%ld failed: %s",
                 d->name ? d->name : "?", (long)value, esp_err_to_name(err));
        return err;
    }
    int32_t actual = cam_ctrl_decode_int(rb, d->value_size);
    if (actual != value) {
        ESP_LOGW(TAG, "SET_CUR verify %s mismatch: req=%ld cur=%ld",
                 d->name ? d->name : "?", (long)value, (long)actual);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

bool cam_ctrl_is_live_safe(const cam_ctrl_desc_t *d)
{
    if (!d) return false;
    /* JPEG quality 会触发 Cam 端重新配置 JPEG 采集路径；streaming 中下发会造成无帧。 */
    if (d->entity_id == UVC_ENTITY_ID_EXTENSION_UNIT && d->selector == UVC_XU_CTRL_MJPEG_HW_QUALITY) {
        return false;
    }
    return true;
}

void cam_ctrl_apply_pending(app_t *a)
{
    if (!a || !a->stream_hdl) return;
    for (size_t i = 0; i < CAM_ADV_CTRL_COUNT; i++) {
        cam_ctrl_desc_t *d = &a->adv_caps[i];
        if (!d->supported || !d->pending_apply) continue;
        // 默认值无需重发
        if (d->v_cur == d->v_def) {
            d->pending_apply = false;
            continue;
        }
        if (cam_ctrl_set_int(a->stream_hdl, d, d->v_cur) == ESP_OK) {
            d->pending_apply = false;
            ESP_LOGI(TAG, "applied pending %s = %ld", d->name, (long)d->v_cur);
        }
    }
}
