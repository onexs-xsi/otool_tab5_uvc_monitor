from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def test_hardware_unsupported_modes_are_listed_not_skipped():
    cam_modes = read("main/uvc/cam_modes.cpp")
    add_mode_body = cam_modes.split("void add_mode", 1)[1].split("int default_mode_index", 1)[0]
    assert "CAM_MAX_SRC_W" not in add_mode_body
    assert "CAM_MAX_SRC_H" not in add_mode_body


def test_ui_uses_current_mode_warning_phrases():
    ui_selection = read("main/ui/ui_selection.cpp")
    readme = read("README_EN.md")
    combined = ui_selection + "\n" + readme
    assert "hardware unsupported" in combined
    assert "not recommended" in combined
    assert "view only" not in combined.lower()


def test_check_skips_non_startable_modes():
    ui_selection = read("main/ui/ui_selection.cpp")
    assert '"SKIP' in ui_selection
    assert "mode_unavailable_reason" in ui_selection


def test_not_recommended_modes_are_selected_then_confirmed_on_start():
    app_internal = read("main/app_internal.h")
    ui_selection = read("main/ui/ui_selection.cpp")
    video_loop = read("main/uvc/video_loop.cpp")
    app_flow = read("main/uvc/app_flow.cpp")
    combined = "\n".join([app_internal, ui_selection, video_loop, app_flow])

    assert "CAM_NOT_RECOMMENDED_CONFIRM_CLICKS" not in combined
    assert "CAM_NOT_RECOMMENDED_CLICK_WINDOW_MS" not in combined
    assert "handle_not_recommended_click" not in ui_selection
    assert "not_recommended_confirmed_mode" in app_internal
    assert "not_recommended_confirm_overlay" in app_internal
    assert "Open video stream?" in ui_selection
    assert "Performance may be poor" in ui_selection
    assert "compressed to fit the display" in ui_selection
    assert "confirm_not_recommended_open_cb" in ui_selection
    assert "show_not_recommended_confirm_dialog(a, a->selected_mode)" in ui_selection
    assert "Press START to confirm opening this not recommended mode" in ui_selection
    assert "mode_is_not_recommended" in combined
    assert "mode_start_requires_confirmation" in combined
    assert "not_recommended_confirmed_mode == mode_idx" in video_loop
    assert "CAM_HIGH_RES_FRAME_CNT" in video_loop


def test_not_recommended_confirm_dialog_font_policy():
    ui_selection = read("main/ui/ui_selection.cpp")
    sdkconfig_defaults = read("sdkconfig.defaults")

    dialog_body = ui_selection.split("static void show_not_recommended_confirm_dialog", 1)[1]
    dialog_body = dialog_body.split("static void confirm_not_recommended_cancel_cb", 1)[0]
    main_title = ui_selection.split('lv_label_set_text(a->title_lbl, "OTool Tab5 UVC Monitor");', 1)[1]
    main_title = main_title.split("lv_obj_align(a->title_lbl", 1)[0]

    assert "&lv_font_montserrat_24" in main_title
    assert 'ui_create_info_label(panel, "Open video stream?",' in dialog_body
    assert "&lv_font_montserrat_24" in dialog_body
    assert "&lv_font_montserrat_16" in dialog_body
    assert "CONFIG_LV_FONT_MONTSERRAT_16=y" in sdkconfig_defaults


if __name__ == "__main__":
    test_hardware_unsupported_modes_are_listed_not_skipped()
    test_ui_uses_current_mode_warning_phrases()
    test_check_skips_non_startable_modes()
    test_not_recommended_modes_are_selected_then_confirmed_on_start()
    test_not_recommended_confirm_dialog_font_policy()
    print("mode listing policy checks passed")
