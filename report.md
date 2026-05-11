# Tab5 UVC Monitor main 目录重组与产线简洁模式修正报告

日期：2026-05-11

## 需求摘要

本次调整围绕产线简洁模式和 `main` 目录结构维护性展开，目标如下：

1. 产线简洁模式右下角版本号应显示 `TAB5_APP_VERSION_BUILD`，即包含构建时间的版本字符串。
2. 左下角实时 FPS 需要保留 2 位小数。
3. 根据现有代码职责，把 `main` 下代码重新组合到 `main/ui` 与 `main/uvc` 目录中，其中 USB/UVC/UAC 相关流程尽量集中到 `main/uvc`。
4. 前提是不删减、不改变现有功能；如有功能删减或改变必须明确告知。
5. 完成分析、代码调整，并记录到本报告。

## 结构分析

调整前 `main` 目录中的文件已经按模块拆过一部分，但 UI、模式选择、视频流程、USB 流程仍在同一层级，随着产线简洁模式、常规选择 UI、音频叠加层和 ADVANCED 叠加层增加，根目录职责开始变得混杂。

当前代码职责可以自然划分为以下几类：

- `main.cpp`：仅保留 `app_main` 入口、产线模式预处理开关，以及对启动/主循环模块的调用。
- `app_runtime.cpp`：板卡识别、TAB5 组件初始化、USB5V 启动、LVGL 显示/触摸初始化和首屏 UI 构建。
- `uvc/*`：USB/UVC/UAC 初始化、摄像头模式枚举、候选模式维护、默认模式选择、产线自动选模策略、REINIT、热插拔状态机、stream 资源和视频帧循环。
- `ui/*`：所有 UI/叠加层相关代码，包括 LVGL 选择界面、产线等待界面、视频阶段 framebuffer overlay、音频 overlay、ADVANCED overlay。
- 根目录保留少量全局入口/共享/显示类模块：`main.cpp`、`app_runtime.cpp`、`app_internal.h`、`display_pipeline.cpp`、`CMakeLists.txt`。

这样的拆分能让后续开发时更容易定位：USB/UVC/UAC 流程看 `main/uvc`，界面和 overlay 看 `main/ui`，板卡/LVGL 启动看 `main/app_runtime.cpp`，入口看 `main/main.cpp`。

## 调整方案

本次采用“移动文件 + 更新 CMake 源文件列表 + 保持函数声明不变”的方式重组，不改公共调用关系，不改变功能入口。

### 新目录职责

- `main/uvc/`
	- 放置 USB/UVC/UAC 初始化、REINIT、UVC 回调、UVC VideoControl、摄像头模式筛选、stream 资源、视频帧循环和主热插拔状态机。
- `main/ui/`
	- 放置 LVGL UI 和视频播放阶段直接绘制到 framebuffer 的 overlay。

### 保留在 `main/` 根目录的文件

- `main.cpp`：只保留 `app_main` 和产线模式宏。
- `app_runtime.cpp`：启动初始化。
- `display_pipeline.cpp`：显示、PPA、panel blit 等显示链路。

## 文件移动清单

以下文件仅移动位置，内部功能保持一致：

- `main/cam_modes.cpp` -> `main/uvc/cam_modes.cpp`
- `main/uvc_callbacks.cpp` -> `main/uvc/uvc_callbacks.cpp`
- `main/usb_task.cpp` -> `main/uvc/usb_task.cpp`
- `main/stream_setup.cpp` -> `main/uvc/stream_setup.cpp`
- `main/video_loop.cpp` -> `main/uvc/video_loop.cpp`
- `main/ui_selection.cpp` -> `main/ui/ui_selection.cpp`
- `main/factory_overlay.cpp` -> `main/ui/factory_overlay.cpp`
- `main/audio_overlay.cpp` -> `main/ui/audio_overlay.cpp`
- `main/advanced_overlay.cpp` -> `main/ui/advanced_overlay.cpp`
- `main/main.cpp` 中的启动初始化和状态机拆分到 `main/app_runtime.cpp` 与 `main/uvc/app_flow.cpp`

同时更新了 `main/CMakeLists.txt` 的 `SRCS` 路径，使 ESP-IDF 能从新目录编译对应源文件。

## 版本号修正

产线简洁模式的等待界面和视频播放 overlay 均已改为显示：

- `TAB5_APP_VERSION_BUILD`

该宏由 `main/CMakeLists.txt` 生成，格式为：

- `v${TAB5_APP_VERSION}_${TAB5_APP_BUILD_TIME}`

示例格式：`v1.0.0_YYYYMMDD_HHMMSS`

为保证视频阶段直接绘制版本号时完整显示，也补充了 `_` 字符的 5x7 点阵字形。

## FPS 修正

产线简洁模式左下角 FPS 显示已从整数改为 2 位小数。

具体调整：

- `app_t::display_fps` 从 `uint32_t` 改为 `float`。
- `video_loop.cpp` 中 1 秒窗口统计仍基于实际渲染帧数，但不再四舍五入成整数。
- `factory_overlay.cpp` 中格式化输出改为 `%.2f`。

这样左下角显示的是实时渲染 FPS，例如：

- `MJPEG 1280x720 FPS 29.97`
- `YUY2 640x480 FPS 15.00`

## 功能删减或行为改变说明

本次未删减功能。

本次有两处按用户要求发生的显示行为变化：

1. 产线简洁模式版本号从 `TAB5_APP_VERSION` 改为 `TAB5_APP_VERSION_BUILD`，因此会显示带构建时间的版本号。
2. 产线简洁模式视频阶段 FPS 从整数显示改为保留 2 位小数显示。

除上述显示内容变化外：

- 产线简洁模式等待、自动选模、打开视频、热拔插循环逻辑保持不变。
- 普通选择 UI、CHECK、REINIT、UAC 音频、ADVANCED 配置和视频叠加层功能保持不变。
- 视频解码、PPA 缩放旋转、panel blit、USB task、UVC/UAC 回调和 stream 资源管理流程保持不变。
- 本次 `main/uvc` 聚合和 `main.cpp` 瘦身仅改变代码组织方式，不改变运行逻辑。

## 验证结果

已使用 ESP-IDF 6.0.1 对重组后的代码进行构建验证。

验证方式：

- 最新验证使用 VS Code ESP-IDF 构建流程，构建目录为 `build/`。
- 构建目标由 `sdkconfig` 推断为 `esp32p4`。

构建结果：

- 构建完成并生成固件镜像。
- 生成固件大小：`0xe64d0` 字节。
- 最小 app 分区大小：`0xf00000` 字节。
- 剩余空间：`0xe19b30` 字节，约 94%。

构建期间观察到的提示均不是本次改动引入：

- `sdkconfig.defaults` 中存在未知 Kconfig 符号 `LVGL_PORT_ENABLE_PPA` 的提示。
- `components/otool_lvgl_idf_port/src/otool_lvgl_port_touch.cpp` 中使用的 `esp_lcd_touch_get_coordinates` 有弃用告警。

VS Code 问题面板仍可能提示 ESP-IDF includePath 相关诊断，这是 IntelliSense 配置层面的提示；实际 ESP-IDF 构建已经通过。

## 后续建议

后续如果继续扩展，建议按以下方向进一步拆分，但本次未做，以避免引入不必要功能风险：

- 如后续继续瘦身，可将 `main/uvc/video_loop.cpp` 内的解码、PPA 变换、统计逻辑进一步拆成更细的 `pipeline` 或 `video` 子模块。
- 将 `app_internal.h` 中的大型结构按职责拆成更小的内部头文件。
- 针对产线简洁模式增加真实设备热拔插长时间循环测试记录。
