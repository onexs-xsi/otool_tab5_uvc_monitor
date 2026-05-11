/*
 * SPDX-FileCopyrightText: 2026 onexs-xsi
 *
 * SPDX-License-Identifier: MIT
 */

#include "app_internal.h"

extern "C" void app_main(void)
{
    if (!app_bootstrap(&s_app, APP_STARTUP_MODE_DEFAULT)) return;
    app_run_main_loop(&s_app);
}
