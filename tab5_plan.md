# Tab5 项目变更计划 (otool_tab5_uvc_monitor)

## 项目背景

ESP32-P4 (M5 TAB5) UVC Host 显示器。通过 USB Host 接收 UVC 视频流并显示在 DSI 屏幕上，
支持可选 UAC 音频。主要逻辑分布在 `main/` 各 `.cpp` 模块，由 `app_internal.h` 共享类型与状态。

---

## 已完成变更

### 1. REINIT 按钮
- **文件**: `main/ui_selection.cpp`
- **内容**: 底部工具栏新增 REINIT 橙色按钮，点击后通过 `app_request_usb_reinit()` 掉电再上电 USB 摄像头，
  用于摄像头卡死时快速恢复。

### 2. 热插拔支持
- **文件**: `main/main.cpp`
- **内容**: SELECTING 状态等待循环内增加 `EV_DEV_CONNECTED` 监听，摄像头重新上电后自动刷新模式列表。

### 3. USB 功耗循环实现
- **文件**: `main/main.cpp`
- **内容**: `app_request_usb_reinit()` 非阻塞实现，创建临时任务，先发 `EV_DEV_LOST | EV_REINIT_REQUESTED`，
  然后控制 USB5V GPIO 关→延时800ms→开。

---

## 计划中变更

### 4. 信息面板视觉重设计

- **文件**: `main/ui_selection.cpp`, `main/app_internal.h`
- **内容**:
  - 右侧信息栏原有 4 个文本块替换为带视觉层次的卡片布局。
  - 模式选中卡：蓝色淡底，含格式 chip 徽章 + 大字分辨率 + fps 文本。
  - 格式统计 chip 行：MJPEG(蓝)/YUY2(绿)/RGBP(橙) 彩色小标签，数量动态更新。
  - 设备行/音频行：简洁单行，带颜色状态指示。
  - `app_t` 新增 `info_mode_fmt_chip`、`info_chip_row`、`info_audio_badge` 等引用字段。

### 5. ADVANCED 悬浮控制面板

- **文件**: `main/ui_selection.cpp`, `main/app_internal.h`
- **内容**:
  - 信息栏 Audio 下方新增 ADVANCED 切换按钮。
  - 点击后在屏幕右侧弹出 320px 宽浮动控制面板（lv_obj 覆盖在最顶层）。
  - 控制面板内容：
    - Contrast 滑块 [-2, 2]（PU, int16_t）
    - Saturation 滑块 [-2, 2]（PU, int16_t）
    - AE Level 滑块 [-5, 5]（XU, int32_t）
    - White Balance 5 个按钮（XU, int32_t, 0=Auto/1=Sun/2=Cloud/3=Off/4=Home）
    - Mirror/Flip 开关（XU, int32_t）
    - Special Effect 下拉框（XU, int32_t, 0-6）
  - 控件回调调用 `cam_ctrl_send_pu_i16()` / `cam_ctrl_send_xu_i32()`。

### 6. UVC VideoControl 控制发送

- **文件**: `main/usb_task.cpp`
- **内容**:
  - 实现 `cam_ctrl_send_pu_i16(app_t*, uint8_t selector, int16_t value)`：
    发送 SET_CUR 到 PU 实体 (entity_id=3)，wLength=2。
  - 实现 `cam_ctrl_send_xu_i32(app_t*, uint8_t selector, int32_t value)`：
    发送 SET_CUR 到 XU 实体 (entity_id=4)，wLength=4。
  - 内部使用 `extern "C"` 前向声明 `uvc_host_usb_ctrl`（仅在私有头中），
    避免修改组件 CMakeLists。
  - 若 `stream_hdl == nullptr` 则静默返回（日志 WARN）。

---

## 架构说明

```
ADVANCED 面板 (ui_selection.cpp)
  cam_ctrl_send_pu_i16(selector, value)  -- Contrast / Saturation
  cam_ctrl_send_xu_i32(selector, value)  -- Mirror/Flip/Effect/AE/JPEG/WB
       ↓
  uvc_host_usb_ctrl(stream_hdl, 0x21, SET_CUR, wValue, wIndex, wLen, data)
       ↓
  USB Host → UVC VC Interface → Cam 端 entity_id 3/4
```

## 选择器常量（与 Cam 端一致）

| 类型 | Selector | 含义 | 值范围 |
|------|----------|------|--------|
| PU (ID=3) | 0x03 | Contrast | [-2, 2] int16_t |
| PU (ID=3) | 0x07 | Saturation | [-2, 2] int16_t |
| XU (ID=4) | 1 | HMirror | [0, 1] int32_t |
| XU (ID=4) | 2 | VFlip | [0, 1] int32_t |
| XU (ID=4) | 3 | Special Effect | [0, 6] int32_t |
| XU (ID=4) | 4 | AE Level | [-5, 5] int32_t |
| XU (ID=4) | 5 | JPEG Quality | [1, 63] int32_t |
| XU (ID=4) | 6 | WB Mode | [0, 4] int32_t |

## 注意事项

- ADVANCED 面板在选择屏上显示；`stream_hdl` 为 NULL 时控制命令静默丢弃。
- 所有运行时打印保持 ASCII；代码注释使用简体中文。
- 不修改 `managed_components/` 或 ESP-IDF SDK 组件。
