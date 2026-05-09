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
- **Capability CHECK**：在真正开始播放前，逐个尝试所有已枚举模式，并对每个模式采样 5 次帧数据。
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
3. 如有需要，可先按 `CHECK`，逐个打开已枚举模式并确认是否有帧到达。
4. 选择一个可启动的模式；如有需要，可启用 `Audio`，并通过 `CONFIGURATION` 预设高级控制参数。
5. 按下 `START`，停止选择界面并进入视频流播放管线。
6. 如果启用了 `Advanced`，并且摄像头暴露了兼容控件，那么播放阶段会常驻右侧高级控制面板。
7. 如果摄像头在开始播放前卡住，可按 `REINIT` 对 USB 电源进行循环重置并重新枚举设备。

---

## 主要控件说明

| UI 元素 | 行为 |
| --- | --- |
| `START` | 启动当前选中的模式并进入流播放阶段 |
| `CHECK` | 打开检测弹窗，对每个已声明模式采样 5 次帧数据 |
| `REINIT` | 在非流播放状态下循环 USB 5V，适合恢复无响应摄像头 |
| `Audio` | 当检测到兼容音频接口时，启用 UAC 麦克风 / 扬声器功能 |
| `Advanced` | 仅用于本人自制的部分 UVC 固件；普通 UVC 摄像头不支持。兼容固件支持时，可让高级控制侧栏在视频播放阶段常驻显示 |
| `CONFIGURATION` | 打开高级控制面板；可安全实时生效的参数会立即应用，其余参数会排队到下次 `START` 时生效 |

---

## 本地 USB 组件说明

本项目继续使用 `components/usb_host_uac` 和 `components/usb_host_uvc` 作为本地组件，不直接依赖 IDF Component Manager 里的官方 UAC/UVC 组件。原因不是两个组件都做了同等规模的功能分叉，而是 UVC 已经包含本项目运行所需的本地扩展；UAC 保持本地化可以让 USB 音视频组件的版本、补丁和构建布局一起固定。

`components/usb_host_uac` 当前已合并到 Espressif `esp-usb` master 提交 `606635bce0799165ad35fcd712fe6e891b385ed7`，组件版本标记为 `1.4.0`。它与官方 1.4.0 基本保持功能一致；本地保留的主要是项目内组件布局和 manifest 适配，避免直接引用 `esp-usb` monorepo 里的 `override_path` 布局，也便于和本地 UVC 一起锁定。上游 1.4.0 中的 UAC 修订已经合入，包括放宽 driver task core id 校验，以及用 64-bit 中间值计算音频 packet size。

`components/usb_host_uvc` 仍以官方 `usb_host_uvc 2.5.0` 为基础，但它不是官方组件的纯拷贝，必须单独保留，差异包括：

- **额外格式枚举**：官方 UVC 只公开 `MJPEG`、`YUY2`、`H264`、`H265` 等格式；本地组件增加 `MJPEG_SW`、`UYVY`、`GRAY8`、`RGBP`，并在 descriptor parser 中识别 `UYVY`、`Y800/GREY`、`RGBP` 以及带软件编码标记的 MJPEG。
- **MJPEG HW/SW 区分**：本地组件会根据 MJPEG format descriptor 的 `bmFlags` 区分 `MJPEG(HW)` 和 `MJPEG(SW)`。这会影响模式列表、质量控制项和调试日志；官方组件会把它们都当成普通 `MJPEG`。
- **VideoControl 控件枚举 API**：本地组件增加 `uvc_host_ctrl_info_t` 和 `uvc_host_get_ctrl_list()`，从 VC descriptor 的 PU/XU `bmControls` 中解析可用控件。选择界面可以在 stream open 前显示 Advanced 控件入口；官方组件没有这个 public API。
- **BULK URB sizing 行为**：部分摄像头会把 `dwMaxPayloadTransferSize` 上报成接近整帧大小。官方路径可能导致 BULK URB 申请过大；本地逻辑在 BULK endpoint 下优先使用调用方配置的 `urb_size`，避免不必要的大块内存占用。
- **ISOC MJPEG 完整帧过滤**：本地 `uvc_isoc.c` 会在回调前检查并裁剪 MJPEG 到 EOI，丢弃缺 SOI/EOI 的不完整帧，并控制异常日志频率，减少上层 JPEG 解码收到坏帧的概率。
- **上游修复回合**：本地 UVC 已合入同一上游提交中的 payload header EOH 校验、`urb_size == 0` 默认 4x MPS 等修订，同时保留上述项目扩展。

---

## 注意事项与限制

- 高于 **1280x720** 的模式仍会显示在列表中供查看，但会被标记为 **view-only**，无法直接启动。
- 高级控制只适配本人自制的一些 UVC 固件，普通 UVC 摄像头不支持；兼容固件还必须声明对应 descriptor，并能正确响应 UVC `GET_*` / `SET_CUR` 事务。
- `MJPEG(HW) Quality` 不属于 live-safe 控件，会延迟到下一次启动流时再应用。
- 流播放已经启动时，`REINIT` 不会生效。
- 当前仓库提交的 `sdkconfig` 已针对这套 TAB5 / ESP32-P4 Demo 进行调优，但最终固件产物名仍沿用 CMake 项目名 `c145_tab5_as_camera_display_demo_idf600`。

---

## 项目结构

```text
main/
  main.cpp              # app_main、USB 电源控制与顶层状态机
  app_internal.h        # 共享类型、常量和 extern 声明
  cam_modes.cpp         # 模式信息辅助函数与选择限制逻辑
  display_pipeline.cpp  # JPEG 解码、PPA 缩放/旋转、面板刷新
  audio_overlay.cpp     # UAC 流控制、波形/录音叠加、触摸轮询
  advanced_overlay.cpp  # 播放阶段直接绘制的 CAMERA ADVANCED 侧栏
  uvc_callbacks.cpp     # UVC / UAC 驱动事件回调
  usb_task.cpp          # USB Host 后台任务，以及 UVC 控件探测/下发辅助函数
  ui_selection.cpp      # LVGL 选择界面与 CHECK / REINIT / CONFIGURATION 交互
  stream_setup.cpp      # 流缓冲申请与释放
  video_loop.cpp        # 主视频帧消费与显示循环

components/
  otool_dev_autodetect/   # 开发板自动识别
  otool_tab5_component/   # TAB5 硬件初始化，面板、触摸、电源
  otool_lvgl_idf_port/    # LVGL 的 IDF 适配层，显示刷新、触摸输入
  usb_host_uvc/           # USB UVC Host 驱动，本项目保留本地扩展
  usb_host_uac/           # USB UAC Host 驱动，已合并上游 1.4.0
```

---

## 配置说明

| 配置项分组 | 作用 |
| --- | --- |
| `CONFIG_UVC_*` | UVC 帧缓冲、URB 大小以及相关任务优先级 |
| `CONFIG_UAC_*` | 音频缓冲深度、采样率行为与播放 / 录音延迟 |
| `CONFIG_LVGL_*` | LVGL 任务栈大小、tick 定时与 UI 响应性 |

像 **1280x720 启动上限**、高级控件默认值等选择与叠加限制，主要定义在 `main/app_internal.h` 和 `main/usb_task.cpp` 中。

---

## 许可证

本项目自有代码采用 MIT License，详见 [LICENSE](LICENSE)。

Copyright (c) 2026 onexs-xsi

第三方组件和上游移植代码保留其各自目录中的许可证声明。
