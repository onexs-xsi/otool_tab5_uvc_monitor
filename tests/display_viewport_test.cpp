#include "../main/display_viewport.h"

#include <cassert>
#include <cstring>

static display_viewport_config_t default_config(bool adv)
{
    return {
        720,
        1280,
        394,
        adv,
    };
}

int main()
{
    display_viewport_config_t cfg = default_config(false);

    display_viewport_t full_hd = display_viewport_calc(1920, 1080, &cfg);
    assert(full_hd.crop_w == 1863);
    assert(full_hd.crop_h == 1048);
    assert(full_hd.crop_x == 28);
    assert(full_hd.crop_y == 16);
    assert(full_hd.scale_units == 11);
    assert(full_hd.scale_den == 16);
    assert(full_hd.out_w == 720);
    assert(full_hd.out_h == 1280);
    assert(full_hd.off_x == 0);
    assert(full_hd.off_y == 0);
    assert(display_viewport_has_crop(&full_hd));
    assert(display_viewport_fills_target(&full_hd));
    char note[192];
    display_viewport_format_summary(&full_hd, note, sizeof(note));
    assert(strcmp(note, "Display: center crop source to 1863x1048 (+28,+16), then scale 11/16 to fill 720x1280.") == 0);

    display_viewport_t hd = display_viewport_calc(1280, 720, &cfg);
    assert(hd.crop_w == 1280);
    assert(hd.crop_h == 720);
    assert(hd.crop_x == 0);
    assert(hd.crop_y == 0);
    assert(hd.scale_units == 16);
    assert(hd.out_w == 720);
    assert(hd.out_h == 1280);
    assert(!display_viewport_has_crop(&hd));
    assert(display_viewport_fills_target(&hd));
    display_viewport_format_summary(&hd, note, sizeof(note));
    assert(strcmp(note, "Display: no crop; scale 16/16 to fill 720x1280.") == 0);

    display_viewport_t svga = display_viewport_calc(800, 600, &cfg);
    assert(svga.crop_w == 800);
    assert(svga.crop_h == 600);
    assert(svga.scale_units == 19);
    assert(svga.out_w == 712);
    assert(svga.out_h == 950);
    assert(svga.off_x == 4);
    assert(svga.off_y == 165);
    assert(!display_viewport_has_crop(&svga));
    assert(!display_viewport_fills_target(&svga));
    display_viewport_format_summary(&svga, note, sizeof(note));
    assert(strcmp(note, "Display: no crop; scale 19/16 to 712x950, centered with black borders.") == 0);

    return 0;
}
