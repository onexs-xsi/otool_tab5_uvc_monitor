#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <vector>

extern "C" {
#include "uvc_types_priv.h"
#include "uvc_frame_priv.h"
#include "esp_private/uvc_stream.h"
void bulk_transfer_callback(usb_transfer_t *transfer);
extern unsigned uvc_test_submissions;
}
#include "../components/usb_host_uvc/host_test/main/streaming/images/test_logo_jpg.hpp"

using Bytes = std::vector<uint8_t>;
static const Bytes image(logo_jpg.begin(), logo_jpg.end());

struct Camera {
    uvc_stream_t stream = {};
    std::vector<Bytes> frames;
    std::vector<uvc_host_dev_event> events;
    std::vector<uvc_host_frame_t *> held;
    bool retain = false;
    bool pause_on_frame = false;

    explicit Camera(size_t capacity = 100 * 1024, uint32_t max_payload = 0)
    {
        stream.constant.cb_arg = this;
        stream.constant.frame_cb = [](const uvc_host_frame_t *frame, void *arg) {
            auto &camera = *static_cast<Camera *>(arg);
            camera.frames.emplace_back(frame->data, frame->data + frame->data_len);
            if (camera.pause_on_frame) camera.stream.dynamic.streaming = false;
            if (camera.retain) camera.held.push_back(const_cast<uvc_host_frame_t *>(frame));
            return !camera.retain;
        };
        stream.constant.stream_cb = [](const uvc_host_stream_event_data_t *event, void *arg) {
            static_cast<Camera *>(arg)->events.push_back(event->type);
        };
        stream.dynamic.streaming = true;
        stream.dynamic.vs_format = logo_jpg_format;
        stream.dynamic.dwMaxPayloadTransferSize = max_payload;
        stream.single_thread.current_frame_id = 2;
        assert(uvc_frame_allocate(&stream, 1, capacity, 0, nullptr) == ESP_OK);
        uvc_frame_format_update(&stream, &logo_jpg_format);
    }

    void release()
    {
        for (auto *frame : held) assert(uvc_host_frame_return(&stream, frame) == ESP_OK);
        held.clear();
    }

    ~Camera()
    {
        release();
        if (stream.dynamic.current_frame) {
            assert(uvc_host_frame_return(&stream, stream.dynamic.current_frame) == ESP_OK);
            stream.dynamic.current_frame = nullptr;
        }
        assert(uvc_frame_are_all_returned(&stream));
        uvc_frame_free(&stream);
    }

    void urb(std::span<const uint8_t> wire, size_t requested, size_t allocation = 0)
    {
        if (!allocation) allocation = requested;
        assert(wire.size() <= requested && requested <= allocation);
        Bytes buffer(allocation, 0);
        std::copy(wire.begin(), wire.end(), buffer.begin());
        auto *transfer = static_cast<usb_transfer_t *>(std::calloc(1, sizeof(usb_transfer_t)));
        new (transfer) usb_transfer_t{
            .data_buffer = buffer.data(), .data_buffer_size = buffer.size(),
            .num_bytes = static_cast<int>(requested), .actual_num_bytes = static_cast<int>(wire.size()),
            .flags = 0, .device_handle = nullptr, .bEndpointAddress = 0,
            .status = USB_TRANSFER_STATUS_COMPLETED, .timeout_ms = 0, .callback = nullptr,
            .context = &stream, .num_isoc_packets = 0,
        };
        bulk_transfer_callback(transfer);
        std::free(transfer);
    }

    void wire(std::span<const uint8_t> bytes, size_t urb_size = 512, bool zlp = true)
    {
        for (size_t offset = 0; offset < bytes.size(); offset += urb_size) {
            urb(bytes.subspan(offset, std::min(urb_size, bytes.size() - offset)), urb_size);
        }
        if (zlp && bytes.size() % urb_size == 0) urb({}, urb_size);
    }
};

static Bytes payload(std::span<const uint8_t> data, bool eof, unsigned fid = 0, bool error = false, size_t header_len = 2)
{
    Bytes wire(header_len, 0);
    auto *header = reinterpret_cast<uvc_payload_header_t *>(wire.data());
    header->bHeaderLength = header_len;
    header->bmHeaderInfo.end_of_header = true;
    header->bmHeaderInfo.frame_id = fid;
    header->bmHeaderInfo.end_of_frame = eof;
    header->bmHeaderInfo.error = error;
    if (header_len == 12) {
        header->bmHeaderInfo.presentation_time = true;
        header->bmHeaderInfo.source_clock_reference = true;
    }
    wire.insert(wire.end(), data.begin(), data.end());
    return wire;
}

static void expect_image(const Camera &camera, size_t count = 1)
{
    assert(camera.frames.size() == count);
    for (const auto &frame : camera.frames) assert(frame == image);
    assert(uvc_frame_are_all_returned(const_cast<uvc_stream_t *>(&camera.stream)));
}

static void eof_payloads()
{
    for (size_t urb_size : {64, 512, 16384}) {
        for (size_t header_len : {2, 12}) {
            Camera camera;
            camera.wire(payload(std::span(image).first(image.size() - 2), false, 0, false, header_len), urb_size);
            assert(camera.frames.empty());
            camera.wire(payload(std::span(image).last(2), true, 0, false, header_len), urb_size);
            expect_image(camera);
            // A whole frame may also have EOF in its first payload, with the same FID.
            camera.wire(payload(image, true, 0, false, header_len), urb_size);
            expect_image(camera, 2);
        }
    }
}

static void deferred_eof_and_zlp()
{
    Camera camera;
    const auto wire = payload(image, true);
    camera.urb(std::span(wire).first(512), 512);
    assert(camera.frames.empty());
    camera.wire(std::span(wire).subspan(512));
    expect_image(camera);

    Camera full;
    Bytes padded_image = image;
    padded_image.resize(8192 - 2, 0);
    const auto full_wire = payload(padded_image, true);
    full.urb(full_wire, 8192);
    assert(full.frames.empty());
    full.urb({}, 512);
    assert(full.frames == std::vector<Bytes>{padded_image});
    full.urb({}, 512); // A stray ZLP after completion must not emit an empty frame.
    assert(full.frames.size() == 1);
}

static void header_only_and_split_soi()
{
    Camera camera;
    camera.wire(payload(image, false));
    assert(camera.frames.empty());
    camera.wire(payload({}, true));
    expect_image(camera);
    camera.wire(payload({}, true));
    expect_image(camera);

    camera.wire(payload(std::span(image).first(1), false, 1));
    camera.wire(payload(std::span(image).subspan(1), true, 1));
    expect_image(camera, 2);
}

static void negotiated_boundaries()
{
    for (size_t max_payload : {128, 1536, 2048}) {
        Camera camera(100 * 1024, max_payload);
        Bytes wire;
        for (size_t offset = 0; offset < image.size(); offset += max_payload - 2) {
            const auto data = std::span(image).subspan(offset, std::min(max_payload - 2, image.size() - offset));
            const auto part = payload(data, offset + data.size() == image.size());
            wire.insert(wire.end(), part.begin(), part.end());
        }
        // No short packet between maximum-sized payloads, including a boundary inside an URB.
        camera.wire(wire, 1024);
        expect_image(camera);
    }
    Bytes padded_image = image;
    padded_image.resize(8192 - 2, 0);
    const auto wire = payload(padded_image, true);
    Camera exact(100 * 1024, 8192);
    exact.urb(wire, 8192);
    // Full last URB completes at the negotiated limit without a ZLP.
    assert(exact.frames == std::vector<Bytes>{padded_image});
}

static void requested_length_not_allocation()
{
    Camera camera;
    Bytes padded_image = image;
    padded_image.resize(8192 - 2, 0);
    const auto wire = payload(padded_image, true);
    camera.urb(wire, 8192, 8192 + 512);
    assert(camera.frames.empty()); // A full request is not short just because its allocation is larger.
    camera.urb({}, 512, 1024);
    assert(camera.frames == std::vector<Bytes>{padded_image});
}

static void fid_and_error_recovery()
{
    Camera camera;
    camera.wire(payload(image, false));
    assert(camera.frames.empty());
    camera.wire(payload(image, true, 1, true));
    expect_image(camera); // The next FID's ERR must not invalidate the old valid frame.
    camera.wire(payload(image, true));
    expect_image(camera, 2);

    Camera error;
    error.wire(payload(std::span(image).first(100), false));
    error.wire(payload(std::span(image).subspan(100), true, 0, true));
    assert(error.frames.empty());
    error.wire(payload(image, true, 1));
    expect_image(error);
}

static void malformed_headers()
{
    for (const Bytes &bad : std::vector<Bytes>{{0x02}, {0x01, 0x82}, {0x0d, 0x82}, {0x02, 0x86}, {0x0c, 0x82}}) {
        Camera camera;
        camera.urb({}, 512);
        camera.wire(bad);
        assert(camera.frames.empty());
        camera.wire(payload(image, true, 1));
        expect_image(camera);
    }
    Camera tiny;
    tiny.wire(payload(std::span(image).first(1), true));
    assert(tiny.frames.empty());
    tiny.wire(payload(image, true, 1));
    expect_image(tiny);
}

static void buffer_recovery()
{
    Camera overflow(8);
    overflow.wire(payload(image, true));
    assert(overflow.frames.empty());
    assert(overflow.events == std::vector<uvc_host_dev_event>{UVC_HOST_FRAME_BUFFER_OVERFLOW});
    const Bytes small = {0xff, 0xd8, 0x11, 0x22, 0xff, 0xd9};
    overflow.wire(payload(small, true, 1));
    assert(overflow.frames == std::vector<Bytes>{small});
    assert(uvc_frame_are_all_returned(&overflow.stream));

    Camera retained;
    retained.retain = true;
    retained.wire(payload(image, true));
    assert(retained.held.size() == 1);
    retained.wire(payload(image, true, 1));
    assert(retained.frames.size() == 1);
    assert(retained.events == std::vector<uvc_host_dev_event>{UVC_HOST_FRAME_BUFFER_UNDERFLOW});
    retained.release();
    retained.retain = false;
    retained.wire(payload(image, true));
    expect_image(retained, 2);
}

static void pause_from_callback()
{
    Camera camera(100 * 1024, image.size() + 2);
    camera.pause_on_frame = true;
    auto wire = payload(image, true);
    const auto next = payload(image, true, 1);
    wire.insert(wire.end(), next.begin(), next.end());
    const unsigned before = uvc_test_submissions;
    camera.urb(wire, wire.size() + 1);
    expect_image(camera);
    assert(uvc_test_submissions == before);
}

int main()
{
    const struct { const char *name; void (*run)(); } cases[] = {
        {"EOF with image tail, 2/12-byte headers, 64/512/16384-byte URBs", eof_payloads},
        {"EOF deferred across URBs and a final ZLP", deferred_eof_and_zlp},
        {"Header-only EOF and SOI split across payloads", header_only_and_split_soi},
        {"Negotiated payload boundaries, coalesced payloads and full URBs", negotiated_boundaries},
        {"Requested transfer length differs from allocation", requested_length_not_allocation},
        {"FID-only completion and per-frame error recovery", fid_and_error_recovery},
        {"Malformed, truncated and empty payload headers", malformed_headers},
        {"Overflow, underflow and retained frame buffers", buffer_recovery},
        {"Pausing from the frame callback", pause_from_callback},
    };
    for (const auto &test : cases) {
        test.run();
        std::printf("PASS: %s\n", test.name);
    }
    std::puts("PASS: all 9 UVC Bulk regression groups (ASan/UBSan)");
}
