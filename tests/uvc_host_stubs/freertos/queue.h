#pragma once
#include "freertos/FreeRTOS.h"
typedef struct test_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned capacity, unsigned item_size);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout);
unsigned uxQueueSpacesAvailable(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);
