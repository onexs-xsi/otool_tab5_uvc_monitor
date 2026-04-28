# otool_tab5_uvc_monitor

<!-- Header image 1 — replace with actual path after uploading -->
![Banner](docs/images/banner1.jpg)

<!-- Header image 2 — replace with actual path after uploading -->
![Preview](docs/images/banner2.jpg)

Real-time UVC camera display demo for **M5 TAB5 (ESP32-P4)**. Streams video from a USB UVC camera to the built-in DSI panel, with optional UAC audio playback/recording and an LVGL-based stream-selection UI.

---

## Features

- **UVC video streaming** — MJPEG, YUY2, and RGB (RGBP) pixel formats
- **Hardware JPEG decoding** via ESP32-P4 HW JPEG decoder
- **PPA (Pixel Processing Accelerator)** scale + 90° rotation for landscape display
- **UAC audio** — speaker playback and microphone recording with waveform scope overlay
- **LVGL 9.x UI** — format/resolution/FPS selection list, capability check modal, status bar
- **Auto panel detection** — supports ST7123, ILI9881C, ST7121 DSI panel variants
- **FreeRTOS** task/queue architecture; frame pipeline runs independently of the UI task

---

## Hardware

| Item | Detail |
|------|--------|
| Board | M5 TAB5 |
| SoC | ESP32-P4 (RISC-V dual-core 400 MHz) |
| Display | DSI 720×1280 (rendered as 1280×720 landscape) |
| Camera | Any USB UVC-compliant camera |
| Audio | USB UAC device (optional) |

---

## Project Structure

```
main/
  main.cpp              # app_main — hardware init & top-level state machine
  app_internal.h        # shared types, constants, extern declarations
  cam_modes.cpp         # format/mode helpers
  display_pipeline.cpp  # PPA scale/rotate, JPEG decode, panel blit
  audio_overlay.cpp     # UAC stream control, audio scope rendering, touch poll
  uvc_callbacks.cpp     # UVC / UAC driver event callbacks
  usb_task.cpp          # USB library task wrapper
  ui_selection.cpp      # LVGL stream-selection UI
  stream_setup.cpp      # buffer allocation / free
  video_loop.cpp        # main video-loop task

components/
  otool_dev_autodetect/   # board auto-detection
  otool_tab5_component/   # TAB5 hardware init (DSI, power, etc.)
  otool_lvgl_idf_port/    # LVGL IDF port (flush, input driver)
  usb_host_uvc/           # USB UVC host driver
  usb_host_uac/           # USB UAC host driver
```

---

## Build & Flash

### Prerequisites

- [ESP-IDF v6.0](https://github.com/espressif/esp-idf) configured in your environment
- USB host-capable target (ESP32-P4 evaluation board / M5 TAB5)

### Build

```powershell
# Windows — load ESP-IDF environment first
. C:/Espressif/frameworks/esp-idf-v6.0/export.ps1

idf.py build
```

### Flash

```bash
idf.py -p <PORT> flash monitor
```

> Default baud rate is 921600. Adjust with `-b 460800` if needed.

---

## Configuration

Key `sdkconfig` options (configurable via `idf.py menuconfig`):

| Option | Description |
|--------|-------------|
| `CONFIG_UVC_*` | UVC driver buffer sizes and task priorities |
| `CONFIG_UAC_*` | UAC ring-buffer depth and sample rate |
| `CONFIG_LVGL_*` | LVGL task stack / tick period |

---

## License

See individual component directories for their respective licenses.
