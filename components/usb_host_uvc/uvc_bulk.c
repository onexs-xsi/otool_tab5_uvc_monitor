/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>

#include "esp_log.h"

#include "uvc_stream.h"
#include "uvc_types_priv.h"
#include "uvc_check_priv.h"
#include "uvc_frame_priv.h"
#include "uvc_critical_priv.h"

static const char *TAG = "uvc-bulk";

static bool bulk_is_mjpeg(const uvc_stream_t *stream)
{
    return stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG ||
           stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG_SW;
}

static void bulk_complete_frame(uvc_stream_t *stream)
{
    UVC_ENTER_CRITICAL();
    uvc_host_frame_t *frame = stream->dynamic.current_frame;
    stream->dynamic.current_frame = NULL;
    const bool deliver = stream->dynamic.streaming && stream->constant.frame_cb && frame &&
                         frame->data_len > 0 && !stream->single_thread.skip_current_frame &&
                         (!bulk_is_mjpeg(stream) || frame->data_len >= 2);
    UVC_EXIT_CRITICAL();

    bool return_frame = true;
    if (deliver) {
        return_frame = stream->constant.frame_cb(frame, stream->constant.cb_arg);
    }
    if (frame && return_frame) {
        uvc_host_frame_return(stream, frame);
    }
}

static void bulk_add_frame_data(uvc_stream_t *stream, const uint8_t *data, size_t len)
{
    if (stream->single_thread.skip_current_frame || len == 0 || !UVC_ATOMIC_LOAD(stream->dynamic.streaming)) {
        return;
    }

    // Header-only payloads must not allocate or deliver an empty video frame.
    uvc_host_frame_t *frame = UVC_ATOMIC_LOAD(stream->dynamic.current_frame);
    if (!frame) {
        frame = uvc_frame_get_empty(stream);
        if (!frame) {
            stream->single_thread.skip_current_frame = true;
            if (stream->constant.stream_cb) {
                const uvc_host_stream_event_data_t event = {.type = UVC_HOST_FRAME_BUFFER_UNDERFLOW};
                stream->constant.stream_cb(&event, stream->constant.cb_arg);
            }
            return;
        }
        UVC_ENTER_CRITICAL();
        stream->dynamic.current_frame = frame;
        UVC_EXIT_CRITICAL();
    }

    const size_t old_len = frame->data_len;
    if (uvc_frame_add_data(frame, data, len) != ESP_OK) {
        stream->single_thread.skip_current_frame = true;
        if (stream->constant.stream_cb) {
            const uvc_host_stream_event_data_t event = {.type = UVC_HOST_FRAME_BUFFER_OVERFLOW};
            stream->constant.stream_cb(&event, stream->constant.cb_arg);
        }
        return;
    }

    // SOI can itself be split across two payloads. Check only once both bytes exist.
    if (bulk_is_mjpeg(stream) && old_len < 2 && frame->data_len >= 2 &&
            (frame->data[0] != JPEG_MARKER || frame->data[1] != JPEG_SOI)) {
        ESP_LOGW(TAG, "invalid MJPEG SOI");
        stream->single_thread.skip_current_frame = true;
    }
}

static void bulk_finish_payload(uvc_stream_t *stream)
{
    const bool eof = stream->single_thread.bulk_payload_eof;
    stream->single_thread.next_bulk_packet = UVC_STREAM_BULK_PACKET_HEADER;
    stream->single_thread.bulk_payload_bytes = 0;
    stream->single_thread.bulk_payload_eof = false;
    if (eof) {
        bulk_complete_frame(stream);
        // Some cameras rely on EOF without toggling FID for the next frame.
        stream->single_thread.current_frame_id = 2;
    }
}

/**
 * A Bulk URB can contain part of a UVC payload or several complete payloads.
 * Only the beginning of each payload has a UVC header. Its EOF flag applies
 * after all of that payload's data, including continuation URBs, is appended.
 * Short USB transfers (including ZLP) and the negotiated maximum payload size
 * delimit payloads; FID changes and EOF delimit video frames.
 */
void bulk_transfer_callback(usb_transfer_t *transfer)
{
    uvc_stream_t *stream = (uvc_stream_t *)transfer->context;
    switch (transfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED:
        break;
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
    case USB_TRANSFER_STATUS_ERROR:
    case USB_TRANSFER_STATUS_OVERFLOW:
    case USB_TRANSFER_STATUS_STALL:
        ESP_ERROR_CHECK(uvc_host_stream_pause(stream));
        break;
    case USB_TRANSFER_STATUS_TIMED_OUT:
    case USB_TRANSFER_STATUS_SKIPPED:
    default:
        assert(false);
    }
    if (!UVC_ATOMIC_LOAD(stream->dynamic.streaming)) {
        return;
    }

    const size_t requested = transfer->num_bytes > 0 ? (size_t)transfer->num_bytes : transfer->data_buffer_size;
    const size_t actual = (size_t)transfer->actual_num_bytes;
    if (transfer->actual_num_bytes < 0 || actual > requested || requested > transfer->data_buffer_size ||
            (actual > 0 && !transfer->data_buffer)) {
        ESP_LOGW(TAG, "invalid Bulk transfer length: actual=%d requested=%u", transfer->actual_num_bytes, (unsigned)requested);
        stream->single_thread.skip_current_frame = true;
        goto resubmit;
    }

    const size_t max_payload = UVC_ATOMIC_LOAD(stream->dynamic.dwMaxPayloadTransferSize);
    size_t offset = 0;
    while (offset < actual && UVC_ATOMIC_LOAD(stream->dynamic.streaming)) {
        if (stream->single_thread.next_bulk_packet == UVC_STREAM_BULK_PACKET_HEADER) {
            const uvc_payload_header_t *header = (const uvc_payload_header_t *)(transfer->data_buffer + offset);
            stream->single_thread.bulk_payload_bytes = 0;
            stream->single_thread.bulk_payload_eof = false;
            stream->single_thread.next_bulk_packet = UVC_STREAM_BULK_PACKET_DATA;

            if (!uvc_frame_payload_header_validate(header, actual - offset) ||
                    (max_payload && header->bHeaderLength > max_payload)) {
                // Discard this payload until its boundary, then try the next header.
                // In particular, never read header fields from a zero/one-byte packet.
                ESP_LOGW(TAG, "invalid UVC Bulk payload header, len=%u", (unsigned)(actual - offset));
                stream->single_thread.skip_current_frame = true;
            } else {
                if (stream->single_thread.current_frame_id != header->bmHeaderInfo.frame_id) {
                    // Complete the old FID before applying the new header's error flag.
                    bulk_complete_frame(stream);
                    stream->single_thread.current_frame_id = header->bmHeaderInfo.frame_id;
                    stream->single_thread.skip_current_frame = false;
                }
                stream->single_thread.skip_current_frame |= header->bmHeaderInfo.error;
                stream->single_thread.bulk_payload_eof = header->bmHeaderInfo.end_of_frame;
                stream->single_thread.bulk_payload_bytes = header->bHeaderLength;
                offset += header->bHeaderLength;
            }
        }

        if (max_payload && stream->single_thread.bulk_payload_bytes == max_payload) {
            bulk_finish_payload(stream);
            continue;
        }
        size_t len = actual - offset;
        if (max_payload && len > max_payload - stream->single_thread.bulk_payload_bytes) {
            len = max_payload - stream->single_thread.bulk_payload_bytes;
        }
        bulk_add_frame_data(stream, transfer->data_buffer + offset, len);
        stream->single_thread.bulk_payload_bytes += len;
        offset += len;

        if (max_payload && stream->single_thread.bulk_payload_bytes == max_payload) {
            bulk_finish_payload(stream);
        }
    }

    if (actual < requested && stream->single_thread.next_bulk_packet == UVC_STREAM_BULK_PACKET_DATA) {
        // A ZLP terminates a payload whose last data URB was completely full.
        bulk_finish_payload(stream);
    }

resubmit:
    if (UVC_ATOMIC_LOAD(stream->dynamic.streaming)) {
        usb_host_transfer_submit(transfer);
    }
}
