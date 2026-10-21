# otool_tab5_uvc_monitor

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

简体中文 | [English](README_EN.md)

基于 **M5 TAB5 (ESP32-P4)** 的实时 UVC 摄像头显示 Demo。固件会在 LVGL 选择界面中枚举 USB 摄像头模式，将所选格式流式显示到内置 DSI 屏幕，并可选启用 UAC 音频，还支持在播放阶段常驻一个直接绘制的高级相机控制侧栏。

| 选择界面 | 带高级控制的流播放界面 |
| --- | --- |
| ![Banner](docs/images/Banner.jpg) | ![Preview](docs/images/Preview.jpg) |

左图：LVGL 模式选择界面，包含 UVC 信息、Audio 开关、Advanced 开关、`CONFIGURATION`、`REINIT`、`CHECK` 和 `START`。右图：视频播放阶段的界面，显示直接绘制到画面上的 `CAMERA ADVANCED` 侧栏。

---

## 功能特性

- **UVC 视频流**：可从已连接摄像头枚举 MJPEG(HW)、MJPEG(SW)、YUY2、UYVY、GRAY8 和 RGBP 模式。
- **ESP32-P4 硬件加速**：使用硬件 JPEG 解码与 PPA 缩放/旋转，在 TAB5 屏幕上以 1280x720 横屏方式显示。
- **LVGL 9.x 选择界面**：包含模式列表、UVC 信息摘要、版本号、Audio 开关、Advanced 开关与操作按钮。
- **Capability CHECK**：在真正开始播放前，逐个尝试推荐范围内且格式受支持的模式；成功打开后尝试取帧 5 次，其余模式标记为 `SKIP`。
- **高级相机控制**：只适配本人自制的一些 UVC 固件，普通 UVC 摄像头不支持；当兼容固件声明并支持对应控件时，可调节 Contrast、Saturation、HMirror、VFlip、Effect、AE Level、White Balance 与 MJPEG 质量。
- **USB REINIT**：可在选择界面手动重置 USB 5V，对卡住的摄像头进行恢复，而无需重启整板。
- **可选 UAC 音频**：支持麦克风 / 扬声器检测，以及流播放阶段的音频叠加显示。
- **支持 Hot-plug 的状态机**：摄像头断开后会回到选择界面，重新插入后自动刷新。
- **自动屏幕面板识别**：支持 ST7123、ILI9881C 与 ST7121 三种 TAB5 面板变体。
- **FreeRTOS 管线化架构**：UI、USB Host、视频流与音频逻辑由独立任务协作完成。

---

## 硬件要求

| 项目 | 说明 |
| --- | --- |
| 开发板 | M5 TAB5 |
| SoC | ESP32-P4，RISC-V 双核 400 MHz |
| 显示屏 | DSI 720x1280 面板，逻辑上以 1280x720 横屏显示 |
| 摄像头 | 兼容 USB UVC 的摄像头 |
| 音频 | 兼容 USB UAC 的麦克风 / 扬声器设备，可选 |

---

## 使用流程

1. 给 TAB5 上电，并等待 USB 摄像头完成枚举。
2. 在选择列表中查看检测到的格式、分辨率与 FPS。
3. 如有需要，可先按 `CHECK`，检查推荐范围内且格式受支持的模式是否有帧到达；不推荐或不支持的模式会跳过。
4. 选择一个可启动的模式；如有需要，可启用 `Audio`，并通过 `CONFIGURATION` 预设高级控制参数。
5. 按下 `START`。标记为 `not recommended` 的模式会先显示裁剪、缩放方案及性能提示，按 `OPEN` 确认后尝试启动；等待首帧期间显示启动状态。
6. 如果启用了 `Advanced`，并且摄像头暴露了兼容控件，那么播放阶段会常驻右侧高级控制面板。
7. 如果摄像头在开始播放前卡住，可按 `REINIT` 对 USB 电源进行循环重置并重新枚举设备。

---

## 主要控件说明

| UI 元素 | 行为 |
| --- | --- |
| `START` | 启动当前选中的模式；`not recommended` 模式需在弹窗中按 `OPEN` 确认 |
| `CHECK` | 检查推荐范围内且格式受支持的模式，成功打开后尝试取帧 5 次；其他模式标记为 `SKIP` |
| `REINIT` | 在非流播放状态下循环 USB 5V，适合恢复无响应摄像头 |
| `Audio` | 当检测到兼容音频接口时，启用 UAC 麦克风 / 扬声器功能 |
| `Advanced` | 仅用于本人自制的部分 UVC 固件；普通 UVC 摄像头不支持。兼容固件支持时，可让高级控制侧栏在视频播放阶段常驻显示 |
| `CONFIGURATION` | 打开高级控制面板；可安全实时生效的参数会立即应用，其余参数会排队到下次 `START` 时生效 |

---

## 编译（ESP-IDF 6.1）

目标为 `esp32p4`，已使用 ESP-IDF `v6.1` 和 GCC `15.2.0` 编译验证。递归克隆项目以取得已锁定的子模块：

```powershell
git clone --recursive https://github.com/onexs-xsi/otool_tab5_uvc_monitor.git
cd otool_tab5_uvc_monitor
```

已有工作区执行 `git submodule update --init --recursive` 即可。子模块使用 HTTPS 地址，无需配置 SSH 密钥。

Windows 可使用 ESP-IDF 6.1 的 PowerShell 终端；手动加载环境时，请按本机安装路径执行 `export.ps1`，例如 `. C:\Espressif\frameworks\esp-idf-v6.1\export.ps1`。按芯片 revision 选择构建配置（可用 `python -m esptool --chip esp32p4 -p COM3 chip-id` 查看）：

- **早期 ESP32-P4**：使用默认的 `sdkconfig.defaults`，执行 `idf.py build`，再执行 `idf.py -p COM3 flash monitor`。
- **ESP32-P4 v3.1 及以上**：叠加 `sdkconfig.defaults.rev3`。本次 TAB5 v3.2 使用此配置，启用 USB Serial/JTAG 日志，并缩减 bootloader 日志以适配分区前的空间：

```powershell
idf.py -B build/rev3 -D SDKCONFIG=build/rev3/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.rev3" build
```

该配置和固件独立生成在 `build/rev3/`，避免沿用旧芯片的 `sdkconfig`。在 TAB5 USB Serial/JTAG 口烧录时，使用本次实测的 watchdog 复位方式，然后连接日志监视器：

```powershell
cd build/rev3
python -m esptool --chip esp32p4 -p COM3 -b 460800 --before default-reset --after watchdog-reset write-flash '@flash_args'
cd ../..
idf.py -B build/rev3 -p COM3 monitor --no-reset
```

将 `COM3` 换成实际端口；烧录和监视时应关闭其他占用该串口的程序。PPA 旋转由 ESP32-P4 端口自动支持，并通过显示初始化配置启用。

`dependencies.lock` 锁定本次构建使用的托管组件：LVGL `9.6.0~1`、USB Host `1.6.0`、i2c_bus `1.5.2`、esp_codec_dev `1.5.11`、esp_lcd_touch `1.2.1` 和 cmake_utilities `0.5.3`。需要重新解析各 manifest 允许的最新版本时，执行 `idf.py update-dependencies`。

默认输出固件为 `build/c145_tab5_as_camera_display_demo_idf600.bin`，v3.1+ 配置输出在 `build/rev3/` 下。LVGL 标志接口、触摸读取和 LCD 帧完成回调已迁移至当前 API，ESP-IDF 6.1 全量编译通过，编译日志为 0 告警、0 错误。

已在 TAB5（ESP32-P4 v3.2）烧录验证 BULK 摄像头：协商模式为 `MJPEG(HW) 1280x720 @30 FPS`，使用 3 个 16 KiB URB。修复 EOF 末包拼接后，画面正常显示，连续两段约 10 秒统计的屏幕帧率为 **11.38 / 11.75 FPS**；该值是实际显示帧率，30 FPS 是摄像头协商值。音频尚未进行本轮实机验证。

执行 `python tests/test_mode_listing_policy.py` 可验证模式列表和确认提示策略，当前 9 项检查全部通过；显示裁剪、缩放和生成文案的原生检查也已通过 ASan/UBSan 验证。

BULK 收帧回归测试使用实际 UVC 驱动、JPEG 样本和 USB / FreeRTOS 桩，在 Linux 或 WSL 中运行（需要 GCC/G++ 和已经解析的托管 USB 组件）：

```sh
python3 tests/run_uvc_bulk_tests.py --idf-path /path/to/esp-idf
```

测试覆盖携带数据的 EOF 末包、跨 URB 负载、ZLP、协商负载边界、FID 切换及缓冲恢复，并启用 ASan/UBSan。

---

## 本地 USB 组件说明

本项目继续使用 `components/usb_host_uac` 和 `components/usb_host_uvc` 作为本地组件，不直接依赖 IDF Component Manager 里的官方 UAC/UVC 组件。原因不是两个组件都做了同等规模的功能分叉，而是 UVC 已经包含本项目运行所需的本地扩展；UAC 保持本地化可以让 USB 音视频组件的版本、补丁和构建布局一起固定。

`components/usb_host_uac` 已同步官方发布版 `1.5.0`（Espressif `esp-usb` 提交 `6d24137e14a4f6c8138662a7a074be16a899c2c6`），包含非整数采样率 TX 包长的分数累加修复，以及按 `bSubframeSize` 计算 USB 音频包长的修复。本地保留项目内组件布局和 manifest 适配，避免直接引用 `esp-usb` monorepo 的 `override_path`。`uac_host_dev_alt_param_t` 新增 `subframe_size` 字段，应用应按字段名访问。

`components/usb_host_uvc` 已合并官方发布版 `2.6.0`（提交 `516d55da0b051843f50eb36cbc7b9c9dda453a53`），包含通用单元/终端控制 API、EP0 重试与 STALL 区分、描述符长度检查，以及无 EOF 摄像头的 FID 边界处理。它仍保留项目扩展：

- **额外格式枚举**：官方 UVC 只公开 `MJPEG`、`YUY2`、`H264`、`H265` 等格式；本地组件增加 `MJPEG_SW`、`UYVY`、`GRAY8`、`RGBP`，并在 descriptor parser 中识别 `UYVY`、`Y800/GREY`、`RGBP` 以及带软件编码标记的 MJPEG。
- **MJPEG HW/SW 区分**：本地组件会根据 MJPEG format descriptor 的 `bmFlags` 区分 `MJPEG(HW)` 和 `MJPEG(SW)`。这会影响模式列表、质量控制项和调试日志；官方组件会把它们都当成普通 `MJPEG`。
- **VideoControl 控件枚举 API**：本地组件增加 `uvc_host_ctrl_info_t` 和 `uvc_host_get_ctrl_list()`，从 VC descriptor 的 PU/XU `bmControls` 中解析可用控件。选择界面可以在 stream open 前显示 Advanced 控件入口；官方组件没有这个 public API。
- **BULK 负载重组与 URB 大小**：优先使用调用方配置的 `urb_size`，并按短传输、ZLP 和协商的负载上限跨 URB 重组；带 EOF 的末包数据全部追加后才提交帧，避免 JPEG 帧尾丢失。
- **ISOC MJPEG 完整帧过滤**：本地 `uvc_isoc.c` 会在回调前检查并裁剪 MJPEG 到 EOI，丢弃缺 SOI/EOI 的不完整帧，并控制异常日志频率，减少上层 JPEG 解码收到坏帧的概率。
- **超时与帧边界保护**：EP0 超时后保留在途传输保护，避免重用或释放尚未完成的 URB；ISOC 的 FID 边界和 EOF 共用 MJPEG 完整性检查，缺少 EOI 的帧仍会丢弃。

---

## 注意事项与限制

- 高于 **1280x720**、但仍在 **1920x1080** 硬件处理范围内的模式标记为 **not recommended**，可以选中并在 `START` 弹窗中按 `OPEN` 确认后尝试启动，实际帧率和稳定性取决于摄像头与系统负载。超过硬件处理上限的模式标记为 **hardware unsupported**，仅供查看，无法启动。
- 高级控制只适配本人自制的一些 UVC 固件，普通 UVC 摄像头不支持；兼容固件还必须声明对应 descriptor，并能正确响应 UVC `GET_*` / `SET_CUR` 事务。
- `MJPEG(HW) Quality` 不属于 live-safe 控件，会延迟到下一次启动流时再应用。
- 流播放已经启动时，`REINIT` 不会生效。
- 当前仓库提交的 `sdkconfig.defaults` 已针对这套 TAB5 / ESP32-P4 Demo 进行调优，最终固件产物名仍沿用 CMake 项目名 `c145_tab5_as_camera_display_demo_idf600`。

---

## 项目结构

```text
main/
  main.cpp              # 仅保留 app_main 与产线模式预处理开关
  app_runtime.cpp       # 板卡识别、TAB5/LVGL 初始化与首屏 UI
  app_internal.h        # 共享类型、常量和 extern 声明
  display_pipeline.cpp  # JPEG 解码、PPA 缩放/旋转、面板刷新
  display_viewport.cpp  # 裁剪、缩放、居中与显示说明计算
  display_viewport.h    # 显示视口类型和接口
  ui/
    ui_selection.cpp      # LVGL 选择界面与 CHECK / REINIT / CONFIGURATION 交互
    factory_overlay.cpp   # 产线等待界面与视频阶段版本/FPS 信息叠加
    audio_overlay.cpp     # UAC 流控制、波形/录音叠加、触摸轮询
    advanced_overlay.cpp  # 播放阶段直接绘制的 CAMERA ADVANCED 侧栏
  uvc/
    app_flow.cpp          # USB/UVC/UAC 初始化、REINIT 与热插拔状态机
    cam_modes.cpp         # 模式信息辅助函数、选择限制与产线自动选模
    uvc_callbacks.cpp     # UVC / UAC 驱动事件回调
    usb_task.cpp          # USB Host 后台任务，以及 UVC 控件探测/下发辅助函数
    stream_setup.cpp      # 流缓冲申请与释放
    video_loop.cpp        # 主视频帧消费与显示循环

components/
  otool_dev_autodetect/   # 开发板自动识别
  otool_tab5_component/   # TAB5 硬件初始化，面板、触摸、电源
  otool_lvgl_idf_port/    # LVGL 的 IDF 适配层，显示刷新、触摸输入
  usb_host_uvc/           # USB UVC Host 驱动，本项目保留本地扩展
  usb_host_uac/           # USB UAC Host 驱动，已合并上游 1.5.0

tests/
  test_mode_listing_policy.py  # 模式列表与确认提示策略检查
  display_viewport_test.cpp    # 显示视口与生成文案的原生回归检查
  uvc_bulk_test.cpp            # BULK 帧重组与缓冲恢复的原生回归检查
  run_uvc_bulk_tests.py        # 编译实际 UVC 驱动并运行 BULK 回归
  uvc_host_stubs/              # 原生测试使用的 USB / FreeRTOS 桩

docs/
  images/                    # README 界面截图
```

组件的使用说明、更新记录、示例与测试位于各自的 `components/` 子目录。`build/`、`managed_components/` 和 `sdkconfig` 由构建工具生成，不纳入版本管理。

---

## 配置说明

| 配置来源 | 作用 |
| --- | --- |
| `CONFIG_UVC_*` | UVC 描述符日志、帧间隔数组容量、负载头检查与 ISOC 诊断 |
| `CONFIG_UAC_*` | UAC 设备 / 采样率列表容量、ISOC URB 数量及每个 URB 的包数 |
| `CONFIG_LV_*` | LVGL 字体、绘制缓冲、日志与标准库适配 |
| `main/app_runtime.cpp` 与 LVGL 端口的 `otool_lvgl_port_cfg_t` | LVGL 任务栈、优先级与 tick 定时 |

应用层帧缓冲数量、URB 大小、音频缓冲，以及 **1280x720 推荐范围**和 **1920x1080 硬件限制**定义在 `main/app_internal.h` 中；高级控件默认值主要位于 `main/uvc/usb_task.cpp`。通过 `idf.py menuconfig`（v3.1+ 配置使用 `idf.py -B build/rev3 menuconfig`）修改 Kconfig 选项，修改默认配置时同步维护对应的 `sdkconfig.defaults` / `sdkconfig.defaults.rev3`。

---

## 许可证

本项目自有代码采用 MIT License，详见 [LICENSE](LICENSE)。

Copyright (c) 2026 onexs-xsi

第三方组件和上游移植代码保留其各自目录中的许可证声明。
