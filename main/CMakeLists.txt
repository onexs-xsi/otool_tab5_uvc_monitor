# SPDX-FileCopyrightText: 2026 onexs-xsi
#
# SPDX-License-Identifier: MIT

idf_component_register(SRCS "main.cpp"
                  "app_runtime.cpp"
                  "uvc/app_flow.cpp"
                  "uvc/cam_modes.cpp"
                  "uvc/uvc_callbacks.cpp"
                  "uvc/usb_task.cpp"
                  "uvc/stream_setup.cpp"
                  "uvc/video_loop.cpp"
                  "ui/ui_selection.cpp"
                  "ui/audio_overlay.cpp"
                  "ui/advanced_overlay.cpp"
                  "ui/factory_overlay.cpp"
                  "display_pipeline.cpp"
                  "display_viewport.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES
                        usb_host_uac
                        usb_host_uvc
                        otool_dev_autodetect
                        otool_tab5_component
                        otool_lvgl_idf_port
                        lvgl__lvgl
                        esp_lcd
                        esp_driver_jpeg
                        usb)

set(TAB5_APP_VERSION "1.2.1" CACHE STRING "OTool Tab5 UVC Monitor firmware version")
string(TIMESTAMP TAB5_APP_BUILD_TIME "%Y%m%d_%H%M%S")
set(TAB5_APP_VERSION_BUILD "v${TAB5_APP_VERSION}_${TAB5_APP_BUILD_TIME}")
option(TAB5_DIAG_DISABLE_UAC_HOST "Skip UAC host install for UVC control-transfer diagnosis" OFF)
option(TAB5_DIAG_FACTORY_AUTO_START "Use factory simple auto-start mode for serial-only diagnosis" OFF)
option(TAB5_DIAG_CYCLE_USB5V_ON_BOOT "Power-cycle USB 5V during boot for camera diagnosis" OFF)
option(TAB5_DIAG_FACTORY_DEFAULT_MODE "Use the normal default selectable mode in factory diagnostic mode" OFF)
option(TAB5_DIAG_SKIP_CAM_CTRL_PROBE "Skip camera extension/processing control probes before stream start" OFF)
set(TAB5_DIAG_FACTORY_MODE_INDEX "-1" CACHE STRING "Diagnostic factory mode index override; -1 uses automatic selection")
set(TAB5_DIAG_STREAM_OPEN_DELAY_MS "0" CACHE STRING "Diagnostic delay before uvc_host_stream_open")
set(TAB5_UVC_FORCE_SETTLE_MS "-1" CACHE STRING "Force UVC stream-open settle delay for all cameras; -1 uses quirks")
set(TAB5_UVC_QUIRK_1D6B_0102_SETTLE_MS "8000" CACHE STRING "Settle delay for slow UVC gadget 1d6b:0102")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TAB5_APP_VERSION="${TAB5_APP_VERSION}"
    TAB5_APP_BUILD_TIME="${TAB5_APP_BUILD_TIME}"
    TAB5_APP_VERSION_BUILD="${TAB5_APP_VERSION_BUILD}"
    TAB5_DIAG_DISABLE_UAC_HOST=$<BOOL:${TAB5_DIAG_DISABLE_UAC_HOST}>
    TAB5_DIAG_FACTORY_AUTO_START=$<BOOL:${TAB5_DIAG_FACTORY_AUTO_START}>
    TAB5_DIAG_CYCLE_USB5V_ON_BOOT=$<BOOL:${TAB5_DIAG_CYCLE_USB5V_ON_BOOT}>
    TAB5_DIAG_FACTORY_DEFAULT_MODE=$<BOOL:${TAB5_DIAG_FACTORY_DEFAULT_MODE}>
    TAB5_DIAG_SKIP_CAM_CTRL_PROBE=$<BOOL:${TAB5_DIAG_SKIP_CAM_CTRL_PROBE}>
    TAB5_DIAG_FACTORY_MODE_INDEX=${TAB5_DIAG_FACTORY_MODE_INDEX}
    TAB5_DIAG_STREAM_OPEN_DELAY_MS=${TAB5_DIAG_STREAM_OPEN_DELAY_MS}
    TAB5_UVC_FORCE_SETTLE_MS=${TAB5_UVC_FORCE_SETTLE_MS}
    TAB5_UVC_QUIRK_1D6B_0102_SETTLE_MS=${TAB5_UVC_QUIRK_1D6B_0102_SETTLE_MS})
