#pragma once
#include <stdio.h>
/* Keep format/argument checks without printing intentionally malformed test inputs. */
#define ESP_LOGE(tag, ...) do { (void)(tag); if (0) fprintf(stderr, __VA_ARGS__); } while (0)
#define ESP_LOGW ESP_LOGE
#define ESP_LOGI ESP_LOGE
#define ESP_LOGD ESP_LOGE
