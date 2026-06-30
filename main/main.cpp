/*
 * SPDX-FileCopyrightText: 2026 onexs-xsi
 *
 * SPDX-License-Identifier: MIT
 */

#include "app_internal.h"

extern "C" void app_main(void)
{
    // app_startup_mode_t mode = TAB5_DIAG_FACTORY_AUTO_START ?
    //                           APP_STARTUP_MODE_FACTORY_SIMPLE :
    //                           APP_STARTUP_MODE_FACTORY_SIMPLE_AUTO_REBOOT;
    app_startup_mode_t mode = APP_STARTUP_MODE_DEFAULT;
    if (!app_bootstrap(&s_app, mode)) return;
    app_run_main_loop(&s_app);
}
