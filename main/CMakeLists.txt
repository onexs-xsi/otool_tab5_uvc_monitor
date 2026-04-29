idf_component_register(SRCS "main.cpp"
                           "cam_modes.cpp"
                           "display_pipeline.cpp"
                           "audio_overlay.cpp"
                           "advanced_overlay.cpp"
                           "uvc_callbacks.cpp"
                           "usb_task.cpp"
                           "ui_selection.cpp"
                           "stream_setup.cpp"
                           "video_loop.cpp"
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

set(TAB5_APP_VERSION "1.0.0" CACHE STRING "OTool Tab5 UVC Monitor firmware version")
string(TIMESTAMP TAB5_APP_BUILD_TIME "%Y%m%d_%H%M%S")
set(TAB5_APP_VERSION_BUILD "v${TAB5_APP_VERSION}_${TAB5_APP_BUILD_TIME}")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TAB5_APP_VERSION="${TAB5_APP_VERSION}"
    TAB5_APP_BUILD_TIME="${TAB5_APP_BUILD_TIME}"
    TAB5_APP_VERSION_BUILD="${TAB5_APP_VERSION_BUILD}")
