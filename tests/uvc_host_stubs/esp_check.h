#pragma once
#include "esp_err.h"
#include "esp_log.h"
#define ESP_RETURN_ON_FALSE(test, error, ...) do { if (!(test)) return (error); } while (0)
#define ESP_RETURN_ON_ERROR(call, ...) do { esp_err_t error_ = (call); if (error_) return error_; } while (0)
