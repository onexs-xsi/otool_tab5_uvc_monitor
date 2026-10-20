"""Run mode listing and confirmation checks with python tests/test_mode_listing_policy.py."""

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
    assert "ui_format_display_viewport_note(a, m, display_note, sizeof(display_note));" in ui_selection
    assert "display_viewport_format_summary(&viewport, out, out_size);" in ui_selection
    assert "m->fps, display_note);" in ui_selection
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


def test_open_keeps_opening_overlay_during_stream_start():
    ui_selection = read("main/ui/ui_selection.cpp")
    open_cb = ui_selection.split("static void confirm_not_recommended_open_cb", 1)[1]
    open_cb = open_cb.split("static void list_item_cb", 1)[0]
    success_path = open_cb.split("a->not_recommended_confirmed_mode = idx;", 1)[1]
    success_path = success_path.split("ui_commit_selected_mode(a);", 1)[0]
    confirm_cb = ui_selection.split("static void btn_confirm_cb", 1)[1]
    confirm_cb = confirm_cb.split("static void btn_check_cb", 1)[0]
    normal_success_path = confirm_cb.split("if (!mode_is_safe(selected))", 1)[1]
    normal_success_path = normal_success_path.split("ui_commit_selected_mode(a);", 1)[0]

    assert "show_stream_opening_overlay(a, idx)" in success_path
    assert "show_stream_opening_overlay(a, a->selected_mode)" in normal_success_path
    assert "ui_clear_not_recommended_confirm" not in success_path
    assert "ui_set_selected_mode_visual" not in success_path
    assert "ui_update_info_panel" not in success_path
    assert "Opening video stream..." in ui_selection


def test_display_pipeline_logs_scale_decision():
    display_pipeline = read("main/display_pipeline.cpp")
    assert "display scale:" in display_pipeline
    assert "adv=%d" in display_pipeline
    assert "src=%ux%u" in display_pipeline
    assert "out=%ux%u" in display_pipeline


def test_stream_open_failure_consumes_not_recommended_confirmation():
    app_flow = read("main/uvc/app_flow.cpp")
    restore = app_flow.split("static void app_restore_selection_after_stream_alloc_fail", 1)[1]
    restore = restore.split("static void app_run_selection_loop", 1)[0]

    assert "a->not_recommended_confirm_mode = -1" in restore
    assert "a->not_recommended_confirmed_mode = -1" in restore
    assert "ui_populate_mode_list(a)" in restore


def test_start_overlay_is_kept_until_first_frame_for_all_modes():
    app_flow = read("main/uvc/app_flow.cpp")
    keep_overlay = app_flow.split("static bool app_keep_start_overlay_until_first_frame", 1)[1]
    keep_overlay = keep_overlay.split("static void app_prepare_stream_restart_state", 1)[0]
    prepare = app_flow.split("static void app_prepare_stream_restart_state", 1)[1]
    prepare = prepare.split("static void app_close_stream_handle", 1)[0]
    selection_phase = app_flow.split("static void app_run_selection_loop", 1)[1]
    selection_phase = selection_phase.split("a->stream_lvgl_ui = false;", 1)[1]
    selection_phase = selection_phase.split("if (!open_stream", 1)[0]

    assert "not_recommended_confirm_overlay != nullptr" in keep_overlay
    assert "mode_start_requires_confirmation" not in keep_overlay
    assert "bool clear_panel" in prepare
    assert "if (clear_panel) panel_clear(a);" in prepare
    assert "keep_start_overlay" in selection_phase
    assert "app_prepare_stream_restart_state(a, !keep_start_overlay)" in selection_phase


if __name__ == "__main__":
    test_hardware_unsupported_modes_are_listed_not_skipped()
    test_ui_uses_current_mode_warning_phrases()
    test_check_skips_non_startable_modes()
    test_not_recommended_modes_are_selected_then_confirmed_on_start()
    test_not_recommended_confirm_dialog_font_policy()
    test_open_keeps_opening_overlay_during_stream_start()
    test_display_pipeline_logs_scale_decision()
    test_stream_open_failure_consumes_not_recommended_confirmation()
    test_start_overlay_is_kept_until_first_frame_for_all_modes()
    print("mode listing policy checks passed")
