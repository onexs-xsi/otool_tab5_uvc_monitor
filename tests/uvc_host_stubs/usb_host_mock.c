#include <stdio.h>
#include "esp_err.h"
#include "uvc_types_priv.h"
#include "uvc_frame_priv.h"
#include "freertos/portmacro.h"

portMUX_TYPE uvc_lock;
unsigned uvc_test_submissions;

struct test_queue {
    unsigned capacity, count, head;
    void **items;
};

QueueHandle_t xQueueCreate(unsigned capacity, unsigned item_size)
{
    assert(capacity && item_size == sizeof(void *));
    QueueHandle_t queue = calloc(1, sizeof(*queue));
    assert(queue);
    queue->capacity = capacity;
    queue->items = calloc(capacity, sizeof(void *));
    assert(queue->items);
    return queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{
    (void)timeout;
    if (queue->count == queue->capacity) return 0;
    queue->items[(queue->head + queue->count++) % queue->capacity] = *(void *const *)item;
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout)
{
    (void)timeout;
    if (!queue->count) return 0;
    *(void **)item = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    return pdPASS;
}

unsigned uxQueueSpacesAvailable(QueueHandle_t queue) { return queue->capacity - queue->count; }
void vQueueDelete(QueueHandle_t queue) { free(queue->items); free(queue); }

void _esp_error_check_failed(esp_err_t error, const char *file, int line, const char *function, const char *expression)
{
    fprintf(stderr, "%s:%d %s: %s failed: %d\n", file, line, function, expression, error);
    abort();
}

esp_err_t usb_host_transfer_submit(usb_transfer_t *transfer)
{
    (void)transfer;
    uvc_test_submissions++;
    return ESP_OK;
}

esp_err_t uvc_host_stream_pause(uvc_host_stream_hdl_t stream)
{
    stream->dynamic.streaming = false;
    if (stream->dynamic.current_frame) {
        uvc_host_frame_return(stream, stream->dynamic.current_frame);
        stream->dynamic.current_frame = NULL;
    }
    return ESP_OK;
}
