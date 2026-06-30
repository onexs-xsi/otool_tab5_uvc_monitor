/*
 * SPDX-FileCopyrightText: 2026 onexs-xsi
 *
 * SPDX-License-Identifier: MIT
 */

#include "app_internal.h"

extern "C" void app_main(void)
{
#if TAB5_DIAG_FACTORY_AUTO_START
    app_startup_mode_t mode = APP_STARTUP_MODE_FACTORY_SIMPLE;
#else
    app_startup_mode_t mode = APP_STARTUP_MODE_DEFAULT;
#endif
    if (!app_bootstrap(&s_app, mode)) return;
    app_run_main_loop(&s_app);
}
