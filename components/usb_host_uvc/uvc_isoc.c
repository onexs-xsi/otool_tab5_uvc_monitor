/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <string.h> // For memcpy

#include "sdkconfig.h"
#include "esp_log.h"

#include "uvc_stream.h" // For uvc_host_stream_pause()
#include "uvc_types_priv.h"
#include "uvc_check_priv.h"
#include "uvc_frame_priv.h"
#include "uvc_critical_priv.h"

static const char *TAG = "uvc-isoc";
static uint32_t s_usb_error_log_count;
#ifdef CONFIG_UVC_CHECK_PAYLOAD_HEADER_ERR
static uint32_t s_frame_error_log_count;
#endif
static uint32_t s_invalid_header_log_count;
static uint32_t s_invalid_mjpeg_soi_log_count;
static uint32_t s_invalid_mjpeg_log_count;
static uint32_t s_mjpeg_soi_recovered_log_count;

#ifndef UVC_ISOC_DIAG_ENABLED
#if CONFIG_UVC_ISOC_DIAG
#define UVC_ISOC_DIAG_ENABLED 1
#else
#define UVC_ISOC_DIAG_ENABLED 0
#endif
#endif

#ifndef UVC_ISOC_DIAG_SAMPLE_LIMIT
#ifdef CONFIG_UVC_ISOC_DIAG_SAMPLE_LIMIT
#define UVC_ISOC_DIAG_SAMPLE_LIMIT CONFIG_UVC_ISOC_DIAG_SAMPLE_LIMIT
#else
#define UVC_ISOC_DIAG_SAMPLE_LIMIT 32U
#endif
#endif

#define UVC_ISOC_DIAG_SUMMARY_MS   1000U
#define UVC_ISOC_MJPEG_SCAN_LIMIT  1024U

static bool is_mjpeg_format(enum uvc_host_stream_format format)
{
    return format == UVC_VS_FORMAT_MJPEG || format == UVC_VS_FORMAT_MJPEG_SW;
}

static int find_jpeg_marker_head(const uint8_t *data, size_t data_len, uint8_t code)
{
    if (!data || data_len < 2) {
        return -1;
    }

    const size_t scan_len = data_len < UVC_ISOC_MJPEG_SCAN_LIMIT ? data_len : UVC_ISOC_MJPEG_SCAN_LIMIT;
    for (size_t i = 1; i < scan_len; i++) {
        if (data[i - 1] == JPEG_MARKER && data[i] == code) {
            return (int)(i - 1);
        }
    }
    return -1;
}

static bool mjpeg_payload_resync_to_soi(const uint8_t **data, size_t *data_len)
{
    if (!data || !*data || !data_len || *data_len < 2) {
        return false;
    }
    if ((*data)[0] == JPEG_MARKER && (*data)[1] == JPEG_SOI) {
        return true;
    }

    const int soi_at = find_jpeg_marker_head(*data, *data_len, JPEG_SOI);
    if (soi_at < 0) {
        return false;
    }

    *data += soi_at;
    *data_len -= (size_t)soi_at;
    return true;
}

#if UVC_ISOC_DIAG_ENABLED

static int find_jpeg_marker_tail(const uint8_t *data, size_t data_len, uint8_t code)
{
    if (!data || data_len < 2) {
        return -1;
    }

    const size_t scan_start = data_len > UVC_ISOC_MJPEG_SCAN_LIMIT ? data_len - UVC_ISOC_MJPEG_SCAN_LIMIT : 0;
    for (size_t i = data_len - 1; i > scan_start; i--) {
        if (data[i - 1] == JPEG_MARKER && data[i] == code) {
            return (int)(i - 1);
        }
    }
    return -1;
}

typedef struct {
    uint32_t transfers;
    uint32_t packets;
    uint32_t payload_packets;
    uint32_t payload_bytes;
    uint32_t sof;
    uint32_t eof;
    uint32_t frame_errors;
    uint32_t invalid_headers;
    uint32_t invalid_mjpeg_soi;
    uint32_t incomplete_mjpeg;
    uint32_t skipped_frames;
    uint32_t skipped_complete_mjpeg;
    uint32_t delivered_frames;
    uint32_t suspect_delivered_frames;
    uint32_t suspect_dropped_frames;
    uint32_t recovered_soi;
    uint32_t usb_errors;
    uint32_t overflows;
    uint32_t underflows;
    uint32_t sample_logs;
    uint32_t max_payload_len;
    uint32_t max_frame_len;
    uint32_t last_summary_ms;
} uvc_isoc_diag_stats_t;

static uvc_isoc_diag_stats_t s_diag;

static bool isoc_diag_take_sample(void)
{
    const uint32_t sample = s_diag.sample_logs++;
    return sample < UVC_ISOC_DIAG_SAMPLE_LIMIT;
}

static void isoc_diag_log_packet_sample(const char *reason, int pkt, const usb_isoc_packet_desc_t *desc,
                                        const uvc_payload_header_t *hdr, const uint8_t *data,
                                        size_t data_len, bool start_of_frame, bool skip_current_frame)
{
    if (!isoc_diag_take_sample()) {
        return;
    }

    const int soi_at = find_jpeg_marker_head(data, data_len, JPEG_SOI);
    const int eoi_at = find_jpeg_marker_tail(data, data_len, 0xD9);
    ESP_LOGW(TAG,
             "diag pkt %s sample=%u pkt=%d status=%d actual=%u hdr=%u info=%02x fid=%u eof=%u err=%u sof=%u skip=%u data_len=%u first=%02x%02x%02x%02x soi_at=%d eoi_at=%d",
             reason,
             (unsigned)s_diag.sample_logs,
             pkt,
             desc ? desc->status : -1,
             desc ? (unsigned)desc->actual_num_bytes : 0,
             hdr ? hdr->bHeaderLength : 0,
             hdr ? hdr->bmHeaderInfo.val : 0,
             hdr ? hdr->bmHeaderInfo.frame_id : 0,
             hdr ? hdr->bmHeaderInfo.end_of_frame : 0,
             hdr ? hdr->bmHeaderInfo.error : 0,
             start_of_frame,
             skip_current_frame,
             (unsigned)data_len,
             data_len > 0 ? data[0] : 0,
             data_len > 1 ? data[1] : 0,
             data_len > 2 ? data[2] : 0,
             data_len > 3 ? data[3] : 0,
             soi_at,
             eoi_at);
}

static void isoc_diag_log_raw_packet_sample(const char *reason, int pkt, const usb_isoc_packet_desc_t *desc,
                                            const uint8_t *packet, size_t packet_len)
{
    if (!isoc_diag_take_sample()) {
        return;
    }

    ESP_LOGW(TAG,
             "diag raw %s sample=%u pkt=%d status=%d actual=%u first=%02x%02x%02x%02x",
             reason,
             (unsigned)s_diag.sample_logs,
             pkt,
             desc ? desc->status : -1,
             (unsigned)packet_len,
             packet_len > 0 ? packet[0] : 0,
             packet_len > 1 ? packet[1] : 0,
             packet_len > 2 ? packet[2] : 0,
             packet_len > 3 ? packet[3] : 0);
}

static void isoc_diag_log_frame_sample(const char *reason, const uvc_host_frame_t *frame, bool frame_complete)
{
    if (!frame || !isoc_diag_take_sample()) {
        return;
    }

    const int soi_at = find_jpeg_marker_head(frame->data, frame->data_len, JPEG_SOI);
    const int eoi_at = find_jpeg_marker_tail(frame->data, frame->data_len, 0xD9);
    ESP_LOGW(TAG,
             "diag frame %s sample=%u len=%u complete=%u first=%02x%02x%02x%02x last=%02x%02x%02x%02x soi_at=%d eoi_at=%d",
             reason,
             (unsigned)s_diag.sample_logs,
             (unsigned)frame->data_len,
             frame_complete,
             frame->data_len > 0 ? frame->data[0] : 0,
             frame->data_len > 1 ? frame->data[1] : 0,
             frame->data_len > 2 ? frame->data[2] : 0,
             frame->data_len > 3 ? frame->data[3] : 0,
             frame->data_len > 3 ? frame->data[frame->data_len - 4] : 0,
             frame->data_len > 2 ? frame->data[frame->data_len - 3] : 0,
             frame->data_len > 1 ? frame->data[frame->data_len - 2] : 0,
             frame->data_len > 0 ? frame->data[frame->data_len - 1] : 0,
             soi_at,
             eoi_at);
}

static void isoc_diag_maybe_log_summary(void)
{
    const uint32_t now_ms = esp_log_timestamp();
    if (s_diag.last_summary_ms == 0) {
        s_diag.last_summary_ms = now_ms;
        return;
    }
    if ((uint32_t)(now_ms - s_diag.last_summary_ms) < UVC_ISOC_DIAG_SUMMARY_MS) {
        return;
    }
    s_diag.last_summary_ms = now_ms;

    ESP_LOGI(TAG,
             "diag totals: xfers=%u pkts=%u payload_pkts=%u bytes=%u sof=%u eof=%u delivered=%u suspect_delivered=%u suspect_dropped=%u recovered_soi=%u skipped=%u skipped_complete=%u invalid_hdr=%u frame_err=%u invalid_soi=%u incomplete=%u usb_err=%u underflow=%u overflow=%u max_payload=%u max_frame=%u",
             (unsigned)s_diag.transfers,
             (unsigned)s_diag.packets,
             (unsigned)s_diag.payload_packets,
             (unsigned)s_diag.payload_bytes,
             (unsigned)s_diag.sof,
             (unsigned)s_diag.eof,
             (unsigned)s_diag.delivered_frames,
             (unsigned)s_diag.suspect_delivered_frames,
             (unsigned)s_diag.suspect_dropped_frames,
             (unsigned)s_diag.recovered_soi,
             (unsigned)s_diag.skipped_frames,
             (unsigned)s_diag.skipped_complete_mjpeg,
             (unsigned)s_diag.invalid_headers,
             (unsigned)s_diag.frame_errors,
             (unsigned)s_diag.invalid_mjpeg_soi,
             (unsigned)s_diag.incomplete_mjpeg,
             (unsigned)s_diag.usb_errors,
             (unsigned)s_diag.underflows,
             (unsigned)s_diag.overflows,
             (unsigned)s_diag.max_payload_len,
             (unsigned)s_diag.max_frame_len);
}
#else
#define isoc_diag_log_packet_sample(reason, pkt, desc, hdr, data, data_len, start_of_frame, skip_current_frame) do { } while (0)
#define isoc_diag_log_raw_packet_sample(reason, pkt, desc, packet, packet_len) do { } while (0)
#define isoc_diag_log_frame_sample(reason, frame, frame_complete) do { } while (0)
#define isoc_diag_maybe_log_summary() do { } while (0)
#endif

static bool mjpeg_frame_trim(uvc_host_frame_t *frame)
{
    if (!frame || !is_mjpeg_format(frame->vs_format.format)) {
        return true;
    }
    if (frame->data_len < 4) {
        return false;
    }
    if (frame->data[0] != JPEG_MARKER || frame->data[1] != JPEG_SOI) {
        return false;
    }
    for (size_t i = frame->data_len - 1; i > 0; i--) {
        if (frame->data[i - 1] == JPEG_MARKER && frame->data[i] == 0xD9) {
            frame->data_len = i + 1;
            return true;
        }
    }
    return false;
}

static void isoc_complete_frame(uvc_stream_t *uvc_stream)
{
    bool return_frame = true; // In case streaming is stopped ATM, we must return the frame

    // Check if the user did not stop the stream in the meantime
    UVC_ENTER_CRITICAL();
    uvc_host_frame_t *this_frame = uvc_stream->dynamic.current_frame;
    uvc_stream->dynamic.current_frame = NULL; // Stop writing more data to this frame
    const bool frame_complete = mjpeg_frame_trim(this_frame);
    const bool skipped_frame = uvc_stream->single_thread.skip_current_frame;
#if UVC_ISOC_DIAG_ENABLED
    const bool suspect_frame = uvc_stream->single_thread.mjpeg_suspect_current_frame;
#endif
#if UVC_ISOC_DIAG_ENABLED
    if (this_frame && this_frame->data_len > s_diag.max_frame_len) {
        s_diag.max_frame_len = (uint32_t)this_frame->data_len;
    }
#endif

    // Determine if we should invoke the frame callback:
    // Only invoke the callback if streaming is active, a frame callback exists,
    // and we have a valid frame to pass to the user.
    const bool invoke_fb_callback = (uvc_stream->dynamic.streaming && uvc_stream->constant.frame_cb && this_frame && !skipped_frame && frame_complete);
    UVC_EXIT_CRITICAL();

    // 注意: ESP_LOGW 会走 vfprintf -> newlib 互斥锁, 在 portENTER_CRITICAL
    // 临界区里调用会触发 lock_acquire_generic -> abort(). 这里改成在
    // 退出临界区之后再打印, 同时 this_frame 已被摘出 (current_frame=NULL),
    // 之后没有其它路径会改写它的字段, 访问安全.
    if (this_frame && !frame_complete) {
#if UVC_ISOC_DIAG_ENABLED
        s_diag.incomplete_mjpeg++;
        if (suspect_frame) {
            s_diag.suspect_dropped_frames++;
        }
#endif
        uint32_t log_count = s_invalid_mjpeg_log_count++;
        if (log_count < 8) {
            ESP_LOGW(TAG, "drop incomplete MJPEG frame #%u: len=%u first=%02x%02x last=%02x%02x",
                     (unsigned)(log_count + 1), (unsigned)this_frame->data_len,
                     this_frame->data_len > 0 ? this_frame->data[0] : 0,
                     this_frame->data_len > 1 ? this_frame->data[1] : 0,
                     this_frame->data_len > 1 ? this_frame->data[this_frame->data_len - 2] : 0,
                     this_frame->data_len > 0 ? this_frame->data[this_frame->data_len - 1] : 0);
        }
        isoc_diag_log_frame_sample("incomplete", this_frame, frame_complete);
    } else if (this_frame && skipped_frame) {
#if UVC_ISOC_DIAG_ENABLED
        s_diag.skipped_frames++;
        if (suspect_frame) {
            s_diag.suspect_dropped_frames++;
        }
        if (frame_complete && is_mjpeg_format(this_frame->vs_format.format)) {
            s_diag.skipped_complete_mjpeg++;
        }
#endif
        isoc_diag_log_frame_sample(frame_complete ? "skipped-complete" : "skipped", this_frame, frame_complete);
    }

    if (invoke_fb_callback) {
#if UVC_ISOC_DIAG_ENABLED
        s_diag.delivered_frames++;
        if (suspect_frame) {
            s_diag.suspect_delivered_frames++;
        }
#endif
        return_frame = uvc_stream->constant.frame_cb(this_frame, uvc_stream->constant.cb_arg);
    }
    if (this_frame && return_frame) {
        // The user has processed the frame in his callback, return it back to empty queue
        uvc_host_frame_return(uvc_stream, this_frame);
    }
}

/**
 * @brief Callback function for handling Isochronous USB transfers from a UVC camera.
 *
 * This function processes isochronous transfer packets, which may contain video frame data. The following key points
 * are handled in the transfer:
 *
 * - **Start of Frame (SoF)**: Detected by a change in Frame ID, which toggles between 0 and 1.
 * - **End of Frame (EoF)**: Signaled in the packet header.
 * - **Transfer Characteristics**:
 *   - **No CRC**: Data corruption is possible.
 *   - **No ACK**: Packets can be missed.
 *   - **Packet Header**: Each packet includes a header used to detect errors, missed packets, and other issues.
 *
 * The callback performs the following tasks:
 * 1. Checks the status of each isochronous packet and handles various USB transfer statuses (e.g., completed,
 *    error, device disconnected).
 * 2. Parses packet headers to detect the start of new frames, handles errors, and manages frame buffers.
 * 3. Aggregates valid data into a frame buffer, ensuring no buffer overflow occurs.
 * 4. Signals the end of a frame and invokes user-defined callbacks if necessary.
 *
 * @param[in] transfer Pointer to the completed USB transfer structure.
 */
void isoc_transfer_callback(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    uvc_stream_t *uvc_stream = (uvc_stream_t *)transfer->context;

    // USB_TRANSFER_STATUS_NO_DEVICE is set in transfer->status.
    // Other error codes are saved in status of each ISOC packet descriptor
    if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_ERROR_CHECK(uvc_host_stream_pause(uvc_stream)); // This should never fail
    }

    if (!UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        return; // If the streaming was turned off, we don't have to do anything
    }

#if UVC_ISOC_DIAG_ENABLED
    s_diag.transfers++;
#endif

    const uint8_t *payload = transfer->data_buffer;
    for (int i = 0; i < transfer->num_isoc_packets; i++) {
        usb_isoc_packet_desc_t *isoc_desc = &transfer->isoc_packet_desc[i];
#if UVC_ISOC_DIAG_ENABLED
        s_diag.packets++;
#endif

        // Check USB status
        switch (isoc_desc->status) {
        case USB_TRANSFER_STATUS_COMPLETED:
            break;
        case USB_TRANSFER_STATUS_NO_DEVICE:
        case USB_TRANSFER_STATUS_CANCELED:
            ESP_ERROR_CHECK(uvc_host_stream_pause(uvc_stream)); // This should never fail
            return; // No need to process the rest
        case USB_TRANSFER_STATUS_ERROR:
        case USB_TRANSFER_STATUS_OVERFLOW:
        case USB_TRANSFER_STATUS_STALL:
        {
#if UVC_ISOC_DIAG_ENABLED
            s_diag.usb_errors++;
#endif
            uint32_t log_count = s_usb_error_log_count++;
            if (log_count < 16 || (log_count % 1000) == 0) {
                ESP_LOGW(TAG, "usb err #%u pkt=%d status=%d actual=%d requested=%d transfer=%d",
                         (unsigned)(log_count + 1), i, isoc_desc->status,
                         isoc_desc->actual_num_bytes, isoc_desc->num_bytes, transfer->status);
            }
            isoc_diag_log_raw_packet_sample("usb-error", i, isoc_desc, payload, isoc_desc->actual_num_bytes);
            uvc_stream->single_thread.skip_current_frame = true;
            goto next_isoc_packet; // Data corrupted
        }

        case USB_TRANSFER_STATUS_TIMED_OUT:
        case USB_TRANSFER_STATUS_SKIPPED:
            goto next_isoc_packet; // Skipped and timed out ISOC transfers are not an issue
        default:
            assert(false);
        }

        if (isoc_desc->actual_num_bytes == 0) {
            goto next_isoc_packet;
        }

        // Check for start of new frame
        const uvc_payload_header_t *payload_header = (const uvc_payload_header_t *)payload;
        if (!uvc_frame_payload_header_validate(payload_header, isoc_desc->actual_num_bytes)) {
#if UVC_ISOC_DIAG_ENABLED
            s_diag.invalid_headers++;
#endif
            uint32_t log_count = s_invalid_header_log_count++;
            if (log_count < 8) {
                ESP_LOGW(TAG, "invalid UVC payload header #%u: %02x %02x len=%d status=%d",
                         (unsigned)(log_count + 1), payload[0], payload[1],
                         isoc_desc->actual_num_bytes, isoc_desc->status);
            }
            isoc_diag_log_raw_packet_sample("invalid-header", i, isoc_desc, payload, isoc_desc->actual_num_bytes);
            uvc_stream->single_thread.skip_current_frame = true;
            goto next_isoc_packet;
        }

        // Derive payload data pointer/length once and reuse below
        const uint8_t *payload_data = payload + payload_header->bHeaderLength;
        size_t payload_data_len = isoc_desc->actual_num_bytes - payload_header->bHeaderLength;
#if UVC_ISOC_DIAG_ENABLED
        s_diag.payload_packets++;
        s_diag.payload_bytes += (uint32_t)payload_data_len;
        if (payload_data_len > s_diag.max_payload_len) {
            s_diag.max_payload_len = (uint32_t)payload_data_len;
        }
#endif

        const bool start_of_frame = (uvc_stream->single_thread.current_frame_id != payload_header->bmHeaderInfo.frame_id);
        const bool end_of_frame = payload_header->bmHeaderInfo.end_of_frame;
        const bool mjpeg_stream = is_mjpeg_format(uvc_stream->dynamic.vs_format.format);

        if (payload_data_len == 0 && !end_of_frame) {
            ESP_LOGD(TAG, "zero-length packet, skipping");
            goto next_isoc_packet;
        }

        // Finish the old frame before applying the new frame's error flags.
        // Cameras without EOF delimit frames only by toggling FID.
        if (start_of_frame) {
            isoc_complete_frame(uvc_stream);
        }

#ifdef CONFIG_UVC_CHECK_PAYLOAD_HEADER_ERR
        // Check for error flag
        if (payload_header->bmHeaderInfo.error) {
            const bool hard_frame_error = !mjpeg_stream;
#if UVC_ISOC_DIAG_ENABLED
            s_diag.frame_errors++;
#endif
            uint32_t log_count = s_frame_error_log_count++;
            if (log_count < 16 || (log_count % 1000) == 0) {
                ESP_LOGW(TAG, "frame error #%u", (unsigned)(log_count + 1));
            }
            isoc_diag_log_packet_sample("frame-error", i, isoc_desc, payload_header, payload_data,
                                        payload_data_len, start_of_frame, uvc_stream->single_thread.skip_current_frame);
            if (hard_frame_error) {
                uvc_stream->single_thread.skip_current_frame = true;
            } else {
                uvc_stream->single_thread.mjpeg_suspect_current_frame = true;
            }
        }
#endif // CONFIG_UVC_CHECK_PAYLOAD_HEADER_ERR

        if (start_of_frame) {
#if UVC_ISOC_DIAG_ENABLED
            s_diag.sof++;
#endif
            // We detected start of new frame. Update Frame ID and start fetching this frame
            uvc_stream->single_thread.current_frame_id   = payload_header->bmHeaderInfo.frame_id;
#ifdef CONFIG_UVC_CHECK_PAYLOAD_HEADER_ERR
            uvc_stream->single_thread.skip_current_frame = !mjpeg_stream && payload_header->bmHeaderInfo.error;
            uvc_stream->single_thread.mjpeg_suspect_current_frame = mjpeg_stream && payload_header->bmHeaderInfo.error;
#else
            uvc_stream->single_thread.skip_current_frame = false;
            uvc_stream->single_thread.mjpeg_suspect_current_frame = false;
#endif

            // Check mjpeg frame start
            if (mjpeg_stream) {
                const uint8_t *const original_payload_data = payload_data;
                const size_t original_payload_data_len = payload_data_len;
                const bool valid_mjpeg_soi = mjpeg_payload_resync_to_soi(&payload_data, &payload_data_len);
                if (valid_mjpeg_soi && payload_data != original_payload_data) {
#if UVC_ISOC_DIAG_ENABLED
                    s_diag.recovered_soi++;
#endif
                    uint32_t log_count = s_mjpeg_soi_recovered_log_count++;
                    if (log_count < 8 || (log_count % 1000) == 0) {
                        ESP_LOGW(TAG, "recovered MJPEG SOI #%u: prefix=%u len=%u first=%02x%02x",
                                 (unsigned)(log_count + 1),
                                 (unsigned)(payload_data - original_payload_data),
                                 (unsigned)original_payload_data_len,
                                 original_payload_data_len > 0 ? original_payload_data[0] : 0,
                                 original_payload_data_len > 1 ? original_payload_data[1] : 0);
                    }
                } else if (!valid_mjpeg_soi) {
#if UVC_ISOC_DIAG_ENABLED
                    s_diag.invalid_mjpeg_soi++;
#endif
                    // We received frame with invalid frame, skip this frame
                    uvc_stream->single_thread.skip_current_frame = true;
                    uint32_t log_count = s_invalid_mjpeg_soi_log_count++;
                    if (log_count < 16 || (log_count % 1000) == 0) {
                        ESP_LOGW(TAG, "invalid MJPEG SOI #%u: len=%u first=%02x%02x",
                                 (unsigned)(log_count + 1), (unsigned)original_payload_data_len,
                                 original_payload_data_len > 0 ? original_payload_data[0] : 0,
                                 original_payload_data_len > 1 ? original_payload_data[1] : 0);
                    }
                    isoc_diag_log_packet_sample("invalid-soi", i, isoc_desc, payload_header, original_payload_data,
                                                original_payload_data_len, start_of_frame, uvc_stream->single_thread.skip_current_frame);
                }
            }

            // Get free frame buffer for this new frame
            UVC_ENTER_CRITICAL();
            const bool need_new_frame = (uvc_stream->dynamic.streaming && !uvc_stream->dynamic.current_frame);
            if (need_new_frame) {
                UVC_EXIT_CRITICAL();
                uvc_stream->dynamic.current_frame = uvc_frame_get_empty(uvc_stream);
                if (uvc_stream->dynamic.current_frame == NULL) {
                    // There is no free frame buffer now, skipping this frame
                    uvc_stream->single_thread.skip_current_frame = true;
#if UVC_ISOC_DIAG_ENABLED
                    s_diag.underflows++;
#endif

                    // Inform the user about the underflow
                    uvc_host_stream_callback_t stream_cb = uvc_stream->constant.stream_cb;
                    if (stream_cb) {
                        const uvc_host_stream_event_data_t event = {
                            .type = UVC_HOST_FRAME_BUFFER_UNDERFLOW,
                        };
                        stream_cb(&event, uvc_stream->constant.cb_arg);
                    }
                    goto next_isoc_packet;
                }
            } else {
                // Streaming was stopped in the meantime: no buffer to write this payload into
                UVC_EXIT_CRITICAL();
                uvc_stream->single_thread.skip_current_frame = true;
            }
        }

        // Add received data to frame buffer
        if (!uvc_stream->single_thread.skip_current_frame) {
            uvc_host_frame_t *current_frame = UVC_ATOMIC_LOAD(uvc_stream->dynamic.current_frame);

            esp_err_t ret = uvc_frame_add_data(current_frame, payload_data, payload_data_len);
            if (ret != ESP_OK) {
                // Frame buffer overflow, skip this frame
                uvc_stream->single_thread.skip_current_frame = true;
#if UVC_ISOC_DIAG_ENABLED
                s_diag.overflows++;
#endif

                // Inform the user about the overflow
                uvc_host_stream_callback_t stream_cb = uvc_stream->constant.stream_cb;
                if (stream_cb) {
                    const uvc_host_stream_event_data_t event = {
                        .type = UVC_HOST_FRAME_BUFFER_OVERFLOW,
                    };
                    stream_cb(&event, uvc_stream->constant.cb_arg);
                }
                goto next_isoc_packet;
            }
        }

        // End of Frame. Pass the frame to user
        if (payload_header->bmHeaderInfo.end_of_frame) {
#if UVC_ISOC_DIAG_ENABLED
            s_diag.eof++;
#endif
            isoc_complete_frame(uvc_stream);
        }
next_isoc_packet:
        payload += isoc_desc->num_bytes;
        continue;
    }

    isoc_diag_maybe_log_summary();

    if (UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        usb_host_transfer_submit(transfer); // Restart the transfer
    }
}
