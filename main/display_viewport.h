/*
 * SPDX-FileCopyrightText: 2026 onexs-xsi
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t panel_w;
    uint32_t panel_h;
    uint32_t adv_reserve_h;
    bool adv_live_enabled;
} display_viewport_config_t;

typedef struct {
    uint32_t src_w;
    uint32_t src_h;
    uint32_t panel_w;
    uint32_t panel_h;
    uint32_t avail_h;
    uint32_t target_w;
    uint32_t target_h;
    uint32_t reserved_top_h;
    uint32_t crop_w;
    uint32_t crop_h;
    uint32_t crop_x;
    uint32_t crop_y;
    uint32_t out_w;
    uint32_t out_h;
    uint32_t off_x;
    uint32_t off_y;
    uint32_t scale_units;
    uint32_t scale_den;
} display_viewport_t;

display_viewport_t display_viewport_calc(uint32_t src_w, uint32_t src_h,
                                         const display_viewport_config_t *config);
bool display_viewport_has_crop(const display_viewport_t *viewport);
bool display_viewport_fills_target(const display_viewport_t *viewport);
void display_viewport_format_summary(const display_viewport_t *viewport,
                                     char *out, size_t out_size);
