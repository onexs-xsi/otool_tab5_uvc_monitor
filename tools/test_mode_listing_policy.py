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


if __name__ == "__main__":
    test_hardware_unsupported_modes_are_listed_not_skipped()
    test_ui_uses_current_mode_warning_phrases()
    test_check_skips_non_startable_modes()
    print("mode listing policy checks passed")
