# 1. IDF version >= 6.0 does not have usb component: usb from IDF component manager will be used
# 2. For linux target, we can't use IDF component manager to get usb component, we need to add it 'the old way'
#    with EXTRA_COMPONENT_DIRS because mocking of managed components is not supported yet.
#    This is acceptable workaround for testing.
set(requires "")
if((${IDF_VERSION_MAJOR} LESS 6) OR ("${IDF_TARGET}" STREQUAL "linux"))
    list(APPEND requires usb)
endif()

idf_component_register(SRCS
                        "uvc_host.c"
                        "uvc_descriptor_parsing.c"
                        "uvc_descriptor_printing.c"
                        "uvc_frame.c"
                        "uvc_control.c"
                        "uvc_isoc.c"
                        "uvc_bulk.c"
                       INCLUDE_DIRS include
                       PRIV_INCLUDE_DIRS private_include include/esp_private
                       PRIV_REQUIRES heap
                       REQUIRES ${requires}
                       )

option(TAB5_DIAG_SKIP_INITIAL_PROBE_GET "Skip the initial UVC VS_PROBE GET_CUR for compatibility diagnosis" OFF)
set(TAB5_DIAG_FORCE_PROBE_LEN "0" CACHE STRING "Force UVC VS probe/commit control length for compatibility diagnosis; 0 uses bcdUVC")
option(TAB5_DIAG_CTRL_TRACE "Log every EP0 control transfer submit/completion" OFF)
option(TAB5_DIAG_UVC_ISOC "Enable UVC ISOC diagnostic logs for compatibility diagnosis" OFF)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TAB5_DIAG_SKIP_INITIAL_PROBE_GET=$<BOOL:${TAB5_DIAG_SKIP_INITIAL_PROBE_GET}>
    TAB5_DIAG_FORCE_PROBE_LEN=${TAB5_DIAG_FORCE_PROBE_LEN}
    TAB5_DIAG_CTRL_TRACE=$<BOOL:${TAB5_DIAG_CTRL_TRACE}>)

if(TAB5_DIAG_UVC_ISOC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE UVC_ISOC_DIAG_ENABLED=1)
endif()
