// 自动从原 main.cpp 拆分而来。仅供 main 组件内部使用。
#include "app_internal.h"

// ===================================================================
// USB host background task
// ===================================================================
void usb_lib_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

