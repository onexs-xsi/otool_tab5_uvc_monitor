// 自动从原 main.cpp 拆分而来。仅供 main 组件内部使用。
#include "app_internal.h"

bool alloc_stream_resources(app_t *a)
{
    bool need_jpeg = false;
    bool need_yuy2 = false;
    bool need_rgbp = false;
    if (a->selected_mode >= 0 && a->selected_mode < (int)a->mode_count) {
        need_jpeg = (a->modes[a->selected_mode].fmt == UVC_VS_FORMAT_MJPEG);
        need_yuy2 = (a->modes[a->selected_mode].fmt == UVC_VS_FORMAT_YUY2);
        need_rgbp = (a->modes[a->selected_mode].fmt == UVC_VS_FORMAT_RGBP);
    }

    if (need_jpeg && !a->decode_buf) {
        jpeg_decode_memory_alloc_cfg_t mcfg = {};
        mcfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
        a->decode_buf = (uint8_t *)jpeg_alloc_decoder_mem(
            DECODE_BUF_SIZE, &mcfg, &a->decode_buf_size);
        if (!a->decode_buf) {
            ESP_LOGE(TAG, "decode buf alloc fail");
            free_stream_resources(a);
            return false;
        }
        memset(a->decode_buf, 0, a->decode_buf_size);
    }

    // YUY2 路径需要把 YUYV 软件转为 RGB565 再交给 PPA（本芯片版本 PPA SRM 不支持 YUYV 输入）
    if (need_yuy2 && !a->decode_buf) {
        const cam_mode_t *m = &a->modes[a->selected_mode];
        size_t need = (size_t)m->w * m->h * 2;
        if (need < 64) need = 64;
        // 64B 对齐，便于 esp_cache_msync 与 PPA DMA 操作
        a->decode_buf = (uint8_t *)heap_caps_aligned_alloc(
            64, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!a->decode_buf) {
            ESP_LOGE(TAG, "yuy2 conv buf alloc fail size=%u", (unsigned)need);
            free_stream_resources(a);
            return false;
        }
        a->decode_buf_size = need;
        memset(a->decode_buf, 0, a->decode_buf_size);
    }

    // RGBP 路径：帧数据已是 RGB565 LE，直接送 PPA，但需 64B 对齐中间 buf
    if (need_rgbp && !a->decode_buf) {
        const cam_mode_t *m = &a->modes[a->selected_mode];
        size_t need = (size_t)m->w * m->h * 2;
        if (need < 64) need = 64;
        a->decode_buf = (uint8_t *)heap_caps_aligned_alloc(
            64, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!a->decode_buf) {
            ESP_LOGE(TAG, "rgbp buf alloc fail size=%u", (unsigned)need);
            free_stream_resources(a);
            return false;
        }
        a->decode_buf_size = need;
        memset(a->decode_buf, 0, a->decode_buf_size);
    }

    if (!a->panel_buf) {
        a->panel_buf = (uint8_t *)heap_caps_aligned_alloc(
            64, PANEL_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!a->panel_buf) {
            ESP_LOGE(TAG, "panel buf alloc fail");
            free_stream_resources(a);
            return false;
        }
        memset(a->panel_buf, 0, PANEL_BUF_SIZE);
    }

    if (!a->ppa) {
        ppa_client_config_t ppa_cfg = {};
        ppa_cfg.oper_type = PPA_OPERATION_SRM;
        esp_err_t err = ppa_register_client(&ppa_cfg, &a->ppa);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ppa_register: %s", esp_err_to_name(err));
            free_stream_resources(a);
            return false;
        }
    }

    if (need_jpeg && !a->jpeg_dec) {
        jpeg_decode_engine_cfg_t eng = {};
        eng.intr_priority = 0;
        eng.timeout_ms    = 500;
        esp_err_t err = jpeg_new_decoder_engine(&eng, &a->jpeg_dec);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "jpeg engine: %s", esp_err_to_name(err));
            free_stream_resources(a);
            return false;
        }
    }

    return true;
}

void free_stream_resources(app_t *a)
{
    if (a->jpeg_dec) {
        jpeg_del_decoder_engine(a->jpeg_dec);
        a->jpeg_dec = nullptr;
    }
    if (a->ppa) {
        ppa_unregister_client(a->ppa);
        a->ppa = nullptr;
    }
    if (a->panel_buf) {
        heap_caps_free(a->panel_buf);
        a->panel_buf = nullptr;
    }
    if (a->decode_buf) {
        heap_caps_free(a->decode_buf);
        a->decode_buf = nullptr;
        a->decode_buf_size = 0;
    }
}



