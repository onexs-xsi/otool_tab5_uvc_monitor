// 内部共享头文件：拆分后的多个 .cpp 模块共享同一份类型/常量与全局状态。
// 仅在本 main 组件内部使用，不对外发布。
#pragma once

#include <cstdio>
#include <cstring>
#include <cmath>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "driver/ppa.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "usb/uac_host.h"
#include "driver/jpeg_decode.h"

#include "otool_lvgl_port.h"
#include "otool_lvgl_port_disp.h"
#include "otool_lvgl_port_touch.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------
#define PANEL_PHYS_W        720
#define PANEL_PHYS_H        1280
// LVGL 中的逻辑坐标（软件旋转 90 度后）：横屏 1280x720
#define PANEL_LOGIC_W       1280
#define PANEL_LOGIC_H       720

// 硬件 JPEG + PPA 可处理的最大源分辨率
#define CAM_MAX_SRC_W       1920
#define CAM_MAX_SRC_H       1080
// 解码缓冲：按最大源分辨率分配 RGB565
#define DECODE_BUF_SIZE     ((size_t)CAM_MAX_SRC_W * CAM_MAX_SRC_H * 2)
// 屏幕缓冲：始终为物理屏大小
#define PANEL_BUF_SIZE      ((size_t)PANEL_PHYS_W * PANEL_PHYS_H * 2)

#define CAM_MAX_CANDIDATES  64
#define CAM_FRAME_CNT       4
#define CAM_QUEUE_DEPTH     4
#define CAM_URB_CNT         3
#define CAM_URB_SIZE        (16 * 1024)
#define CAM_SAFE_MAX_PIXELS (640u * 480u)
#define CAM_FRAME_TIMEOUT_MS 2000
#define CAM_NO_FRAME_DIAG_TIMEOUTS 3
#define CAM_CHECK_ATTEMPTS  5
#define CAM_CHECK_FRAME_TIMEOUT_MS 1500

#define AUDIO_SCOPE_W       360
#define AUDIO_SCOPE_H       96
#define AUDIO_SCOPE_X       24
#define AUDIO_SCOPE_Y       (PANEL_LOGIC_H - AUDIO_SCOPE_H - 24)
#define AUDIO_REC_BOX_W     AUDIO_SCOPE_W
#define AUDIO_REC_BOX_H     AUDIO_SCOPE_H
#define AUDIO_REC_BOX_X     (AUDIO_SCOPE_X + AUDIO_SCOPE_W + 10)
#define AUDIO_REC_BOX_Y     AUDIO_SCOPE_Y
#define AUDIO_REC_BTN_SIZE  68
#define AUDIO_REC_MAX_MS    5000
#define AUDIO_REC_LONG_MS   2000
#define UAC_AUDIO_BUF_SIZE  (16 * 1024)
#define UAC_AUDIO_THRESHOLD (2 * 1024)
#define UAC_READ_BUF_SIZE   2048
#define UAC_WRITE_CHUNK_SIZE 2048
#define UAC_WRITE_TIMEOUT_MS 1000

#define USB_LIB_TASK_PRIO   15
#define UVC_DECODE_TASK_PRIO 10
#define UAC_AUDIO_TASK_PRIO 9

// 事件位
#define EV_DEV_CONNECTED    (1 << 0)
#define EV_USER_CONFIRMED   (1 << 1)
#define EV_DEV_LOST         (1 << 2)
#define EV_UAC_CONNECTED    (1 << 3)
// 用户在选择界面或主界面请求重新上电 USB 摄像头（仅当未在 streaming 状态时生效）
#define EV_REINIT_REQUESTED (1 << 4)

#define IV_TO_FPS(iv)       (((iv) > 0) ? (10000000.0f / (float)(iv)) : 15.0f)

// ---------------------------------------------------------------------------
// 类型
// ---------------------------------------------------------------------------
typedef struct {
    unsigned w;
    unsigned h;
    float    fps;
    enum uvc_host_stream_format fmt;
} cam_mode_t;

typedef enum {
    APP_STATE_WAITING_USB,
    APP_STATE_SELECTING,
    APP_STATE_STREAMING,
} app_state_t;

typedef enum {
    AUDIO_REC_IDLE,
    AUDIO_REC_RECORDING,
    AUDIO_REC_READY,
    AUDIO_REC_PLAYING,
} audio_rec_state_t;

typedef struct {
    // 状态
    volatile app_state_t    state;
    EventGroupHandle_t      events;

    // UVC 设备信息
    uint8_t                 dev_addr;
    uint8_t                 stream_idx;
    cam_mode_t              modes[CAM_MAX_CANDIDATES];
    size_t                  mode_count;

    // 当前流参数
    int                     selected_mode;
    uvc_host_stream_hdl_t   stream_hdl;
    volatile bool           stream_active;
    QueueHandle_t           frame_queue;

    // UAC 音频状态
    volatile bool           uac_driver_ready;
    volatile bool           uac_rx_available;
    volatile bool           uac_tx_available;
    volatile bool           uac_tx_start_attempted;
    volatile bool           uac_selected;
    uint8_t                 uac_addr;
    uint8_t                 uac_iface_num;
    uint8_t                 uac_tx_addr;
    uint8_t                 uac_tx_iface_num;
    uac_host_device_handle_t uac_dev_hdl;
    uac_host_device_handle_t uac_spk_dev_hdl;
    TaskHandle_t            audio_task;
    TaskHandle_t            audio_playback_task;
    SemaphoreHandle_t       audio_rec_mutex;
    volatile bool           audio_task_running;
    volatile bool           audio_active;
    volatile bool           audio_play_stop_requested;
    uint8_t                 audio_channels;
    uint8_t                 audio_bit_resolution;
    uint32_t                audio_sample_freq;
    uint8_t                 speaker_channels;
    uint8_t                 speaker_bit_resolution;
    uint32_t                speaker_sample_freq;
    uint8_t                 audio_scope[AUDIO_SCOPE_W];
    uint8_t                 audio_record_wave[AUDIO_SCOPE_W];
    uint8_t                *audio_record_buf;
    size_t                  audio_record_capacity;
    volatile size_t         audio_record_size;
    uint32_t                audio_record_bytes_per_sec;
    uint32_t                audio_record_duration_ms;
    uint32_t                audio_record_started_ms;
    uint32_t                audio_play_started_ms;
    volatile audio_rec_state_t audio_rec_state;
    bool                    audio_rec_touch_down;
    bool                    audio_rec_long_sent;
    uint32_t                audio_rec_touch_start_ms;

    // 硬件资源
    esp_lcd_panel_handle_t  panel;
    jpeg_decoder_handle_t   jpeg_dec;
    ppa_client_handle_t     ppa;
    uint8_t                *decode_buf;
    size_t                  decode_buf_size;
    uint8_t                *panel_buf;
    lv_display_t           *lv_disp;
    lv_indev_t             *lv_touch_indev;
    esp_lcd_touch_handle_t  touch_handle;

    // 选择 UI 控件
    lv_obj_t               *sel_screen;
    lv_obj_t               *title_lbl;
    lv_obj_t               *list_obj;
    lv_obj_t               *uac_switch;
    lv_obj_t               *uac_lbl;
    lv_obj_t               *confirm_btn;
    lv_obj_t               *check_btn;
    lv_obj_t               *reinit_btn;
    lv_obj_t               *status_lbl;
    lv_obj_t               *info_mode_lbl;
    lv_obj_t               *info_device_lbl;
    lv_obj_t               *info_formats_lbl;
    lv_obj_t               *info_audio_lbl;
    lv_obj_t               *check_overlay;
    lv_obj_t               *check_list;
    lv_obj_t               *check_status_lbl;
    lv_obj_t               *check_close_btn;
    volatile bool           check_running;
    TaskHandle_t            check_task;

    // FPS 统计
    uint32_t                decoded_frames;
    uint32_t                fps_report_ms;
    uint32_t                display_fps;
} app_t;

// ---------------------------------------------------------------------------
// 全局状态（在 main.cpp 中定义）
// ---------------------------------------------------------------------------
extern const char *TAG;
extern app_t s_app;
extern portMUX_TYPE s_audio_scope_lock;

// ---------------------------------------------------------------------------
// 跨文件函数声明
// ---------------------------------------------------------------------------
// cam_modes.cpp
const char *uvc_format_name(enum uvc_host_stream_format fmt);
const char *uvc_stream_event_name(enum uvc_host_dev_event event);
void log_frame_info(const uvc_host_frame_info_t *fi, size_t index);
bool mode_is_safe(const cam_mode_t *m);
void add_mode(app_t *a, unsigned w, unsigned h,
              float fps, enum uvc_host_stream_format fmt);
int  default_mode_index(const app_t *a);

// display_pipeline.cpp
esp_err_t ppa_scale_rotate(app_t *a, const void *src_buf, size_t src_size,
                           unsigned sw, unsigned sh, ppa_srm_color_mode_t in_cm);
esp_err_t panel_blit(app_t *a);
void      panel_clear(app_t *a);
size_t    mjpeg_valid_len(const uint8_t *data, size_t len);
void      yuyv_to_rgb565(const uint8_t *src, uint16_t *dst, size_t pixel_pairs);

// audio_overlay.cpp
void draw_audio_overlay(app_t *a);
void audio_record_touch_poll(app_t *a);
bool start_uac_stream(app_t *a);
void stop_uac_stream(app_t *a);
void uac_device_event_cb(uac_host_device_handle_t uac_device_handle,
                         const uac_host_device_event_t event, void *arg);

// uvc_callbacks.cpp
bool frame_cb(const uvc_host_frame_t *frame, void *user_ctx);
void stream_event_cb(const uvc_host_stream_event_data_t *event, void *user_ctx);
void uvc_driver_event_cb(const uvc_host_driver_event_data_t *event, void *user_ctx);
void uac_driver_event_cb(uint8_t addr, uint8_t iface_num,
                         const uac_host_driver_event_t event, void *arg);

// usb_task.cpp
void usb_lib_task(void *arg);

// main.cpp - 由 UI 触发的 USB 摄像头电源循环（VBUS 关 -> 延时 -> 开），
// 用于卡死或主控复位后的快速恢复；非阻塞，会创建一个一次性辅助任务。
void app_request_usb_reinit(app_t *a);

// ui_selection.cpp
void ui_build_selection(app_t *a);
void ui_style_mode_item(lv_obj_t *item, bool selected);
void ui_populate_mode_list(app_t *a);
void ui_clear_mode_list(app_t *a);
void ui_set_status(app_t *a, const char *txt);
void ui_update_uac_option(app_t *a);
void ui_update_info_panel(app_t *a);

// stream_setup.cpp
bool alloc_stream_resources(app_t *a);
void free_stream_resources(app_t *a);

// video_loop.cpp
void drain_queue(app_t *a);
bool open_stream(app_t *a, int mode_idx);
void video_loop(app_t *a);
