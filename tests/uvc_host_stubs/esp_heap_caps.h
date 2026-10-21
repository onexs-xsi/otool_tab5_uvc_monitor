#pragma once
#include <stdlib.h>
#include <stdint.h>
#define MALLOC_CAP_DEFAULT 0
static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}
