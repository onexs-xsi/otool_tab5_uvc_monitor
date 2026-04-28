idf_component_register(SRCS "main.cpp"
                           "cam_modes.cpp"
                           "display_pipeline.cpp"
                           "audio_overlay.cpp"
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
