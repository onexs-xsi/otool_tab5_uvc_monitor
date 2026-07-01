/*
 * SPDX-FileCopyrightText: 2026 onexs-xsi
 *
 * SPDX-License-Identifier: MIT
 */

#include "display_viewport.h"

#include <stdio.h>

static constexpr uint32_t DISPLAY_VIEWPORT_SCALE_DEN = 16;
static constexpr uint32_t DISPLAY_VIEWPORT_SCALE_UNITS_MAX = DISPLAY_VIEWPORT_SCALE_DEN * 2;

static uint32_t ceil_div_u32(uint64_t n, uint32_t d)
{
    return d ? (uint32_t)((n + d - 1) / d) : 0;
}

static uint32_t clamp_scale_units(uint32_t units)
{
    if (units < 1) return 1;
    if (units > DISPLAY_VIEWPORT_SCALE_UNITS_MAX) return DISPLAY_VIEWPORT_SCALE_UNITS_MAX;
    return units;
}

static uint32_t scale_units_floor(float scale)
{
    if (scale < (1.0f / (float)DISPLAY_VIEWPORT_SCALE_DEN)) {
        return 1;
    }
    return clamp_scale_units((uint32_t)(scale * (float)DISPLAY_VIEWPORT_SCALE_DEN));
}

static uint32_t scale_units_ceil_ratio(uint32_t target_dim, uint32_t src_dim)
{
    if (target_dim == 0 || src_dim == 0) {
        return 1;
    }
    return clamp_scale_units(ceil_div_u32((uint64_t)target_dim * DISPLAY_VIEWPORT_SCALE_DEN, src_dim));
}

static uint32_t scaled_dim(uint32_t src_dim, uint32_t scale_units)
{
    return (uint32_t)(((uint64_t)src_dim * scale_units) / DISPLAY_VIEWPORT_SCALE_DEN);
}

static uint32_t crop_dim_for_target(uint32_t src_dim, uint32_t target_dim, uint32_t scale_units)
{
    if (src_dim == 0 || target_dim == 0 || scale_units == 0) {
        return 0;
    }

    uint32_t crop_dim = (uint32_t)((((uint64_t)target_dim + 1) * DISPLAY_VIEWPORT_SCALE_DEN - 1) / scale_units);
    if (crop_dim > src_dim) {
        crop_dim = src_dim;
    }
    while (crop_dim > 1 && scaled_dim(crop_dim, scale_units) > target_dim) {
        crop_dim--;
    }
    return crop_dim ? crop_dim : 1;
}

display_viewport_t display_viewport_calc(uint32_t src_w, uint32_t src_h,
                                         const display_viewport_config_t *config)
{
    display_viewport_t v = {};
    v.src_w = src_w;
    v.src_h = src_h;
    v.scale_den = DISPLAY_VIEWPORT_SCALE_DEN;

    if (!config || src_w == 0 || src_h == 0 || config->panel_w == 0 || config->panel_h == 0) {
        return v;
    }

    v.panel_w = config->panel_w;
    v.panel_h = config->panel_h;
    v.reserved_top_h = (config->adv_live_enabled && config->adv_reserve_h < config->panel_h)
                       ? config->adv_reserve_h
                       : 0;
    v.avail_h = config->panel_h - v.reserved_top_h;
    v.target_w = config->panel_w;
    v.target_h = v.avail_h;

    float scale_w = (float)v.target_w / (float)src_h;
    float scale_h = (float)v.target_h / (float)src_w;
    float ideal_scale = (scale_w < scale_h) ? scale_w : scale_h;
    if (ideal_scale > 2.0f) ideal_scale = 2.0f;
    if (ideal_scale < 0.1f) ideal_scale = 0.1f;

    v.crop_w = src_w;
    v.crop_h = src_h;
    v.crop_x = 0;
    v.crop_y = 0;
    v.scale_units = scale_units_floor(ideal_scale);

    const uint32_t ideal_out_w = ((uint32_t)((float)src_h * ideal_scale + 0.5f)) & ~1u;
    const uint32_t ideal_out_h = ((uint32_t)((float)src_w * ideal_scale + 0.5f)) & ~1u;
    const bool intended_fullscreen = (ideal_out_w == v.target_w && ideal_out_h == v.target_h);

    if (intended_fullscreen) {
        const uint32_t cover_units_w = scale_units_ceil_ratio(v.target_w, src_h);
        const uint32_t cover_units_h = scale_units_ceil_ratio(v.target_h, src_w);
        v.scale_units = (cover_units_w > cover_units_h) ? cover_units_w : cover_units_h;

        v.crop_h = crop_dim_for_target(src_h, v.target_w, v.scale_units);
        v.crop_w = crop_dim_for_target(src_w, v.target_h, v.scale_units);
        v.crop_x = (src_w > v.crop_w) ? ((src_w - v.crop_w) / 2) : 0;
        v.crop_y = (src_h > v.crop_h) ? ((src_h - v.crop_h) / 2) : 0;
    }

    v.out_w = scaled_dim(v.crop_h, v.scale_units);
    v.out_h = scaled_dim(v.crop_w, v.scale_units);
    v.off_x = (v.target_w > v.out_w) ? ((v.target_w - v.out_w) / 2) : 0;
    v.off_y = v.reserved_top_h + ((v.target_h > v.out_h) ? ((v.target_h - v.out_h) / 2) : 0);
    return v;
}

bool display_viewport_has_crop(const display_viewport_t *viewport)
{
    return viewport &&
           (viewport->crop_w < viewport->src_w || viewport->crop_h < viewport->src_h);
}

bool display_viewport_fills_target(const display_viewport_t *viewport)
{
    return viewport &&
           viewport->out_w == viewport->target_w &&
           viewport->out_h == viewport->target_h &&
           viewport->off_x == 0 &&
           viewport->off_y == viewport->reserved_top_h;
}

void display_viewport_format_summary(const display_viewport_t *viewport,
                                     char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!viewport || viewport->src_w == 0 || viewport->src_h == 0) {
        snprintf(out, out_size, "Display: unavailable.");
        return;
    }

    if (display_viewport_has_crop(viewport)) {
        snprintf(out, out_size,
                 "Display: center crop source to %ux%u (+%u,+%u), then scale %u/%u to fill %ux%u.",
                 (unsigned)viewport->crop_w, (unsigned)viewport->crop_h,
                 (unsigned)viewport->crop_x, (unsigned)viewport->crop_y,
                 (unsigned)viewport->scale_units, (unsigned)viewport->scale_den,
                 (unsigned)viewport->target_w, (unsigned)viewport->target_h);
    } else if (display_viewport_fills_target(viewport)) {
        snprintf(out, out_size,
                 "Display: no crop; scale %u/%u to fill %ux%u.",
                 (unsigned)viewport->scale_units, (unsigned)viewport->scale_den,
                 (unsigned)viewport->target_w, (unsigned)viewport->target_h);
    } else {
        snprintf(out, out_size,
                 "Display: no crop; scale %u/%u to %ux%u, centered with black borders.",
                 (unsigned)viewport->scale_units, (unsigned)viewport->scale_den,
                 (unsigned)viewport->out_w, (unsigned)viewport->out_h);
    }
}
