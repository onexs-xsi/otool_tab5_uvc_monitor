# uvc-isoc MJPEG 适配与修复报告

## 目标

本次目标不是把 `uvc-isoc` 的异常直接放过，而是在 MJPEG isochronous 流中区分“可恢复的异常”和“必须丢弃的坏帧”。

核心原则：

- 保留“不把明显损坏帧交给上层 JPEG 解码器”的安全边界。
- 对仅带 UVC payload header error bit、但 USB packet status 正常且最终 MJPEG 字节完整的帧，允许继续组帧，并在 EOF 时再做最终判定。
- 对 USB packet status 错误、payload header 无效、缓冲区 underflow/overflow、缺失 SOI/EOI 的帧继续丢弃。
- 增加可关闭的诊断日志，用实机串口数据判断后续完善难度。

## 原始现象

用户给出的关键日志：

```text
Stream started: MJPEG(HW) 1920x1080 @15.0 fps
invalid MJPEG SOI #1: len=3048 first=b909
frame error #1
drop incomplete MJPEG frame #1: len=0 first=0000 last=0000
drop incomplete MJPEG frame #2: len=3048 first=ffd8 last=6c38
display stats: 5.62 fps ... mode=MJPEG(HW) 1920x1080
```

这说明至少有三类问题需要分开判断：

- 某些新帧的首个 payload 不是从 `FFD8` SOI 开始。
- 某些帧以 `FFD8` 开始，但到 EOF 或丢弃时没有 `FFD9` EOI。
- 1920x1080@15 的显示链路吞吐较低，但这和 USB payload 重组是否正确不是同一个问题。

## 根因判断

实机诊断显示，本相机在 `MJPEG(HW) 1920x1080 @15.0 fps` 下会频繁设置 UVC payload header error bit，但 USB packet status 仍为正常完成。

旧策略遇到 UVC error bit 会立刻设置 `skip_current_frame`。这能保护上层解码器，但也会导致一些实际完整的 MJPEG 帧被提前放弃。实机数据证明存在这类帧，因为修复后 `suspect_delivered=62`，且 `skipped_complete=0`。

同时，缺失 EOI 的帧仍然存在，不能直接放过。因此修复方向是 `suspect-but-complete`：遇到 UVC error bit 时先标记当前帧可疑，继续组帧；到 EOF 时只有 SOI/EOI 完整且没有硬错误，才交付给 `frame_cb`。

## 已实施的代码变更

修改文件：

- `components/usb_host_uvc/uvc_isoc.c`
- `components/usb_host_uvc/uvc_host.c`
- `components/usb_host_uvc/private_include/uvc_types_priv.h`
- `components/usb_host_uvc/Kconfig`
- `components/usb_host_uvc/CMakeLists.txt`
- `components/usb_host_uvc/host_test/main/streaming/test_streaming.cpp`
- `components/usb_host_uvc/host_test/main/streaming/test_streaming_helpers.hpp`
- `main/uvc/app_flow.cpp`

主要实现：

- 新增 `CONFIG_UVC_ISOC_DIAG` 和 `CONFIG_UVC_ISOC_DIAG_SAMPLE_LIMIT`，默认关闭。
- 新增 `TAB5_DIAG_UVC_ISOC=ON` 诊断构建开关，便于临时打开 `uvc-isoc` 统计日志。
- 新增 `TAB5_DIAG_FACTORY_MODE_INDEX=4` 自动确认逻辑，可在 factory diagnostic mode 中无人值守启动 not recommended 的 `MJPEG(HW) 1920x1080 @15.0 fps`。
- MJPEG SoF payload 如果不是从 `FFD8` 开始，会在最多 1024 字节窗口内扫描 SOI；找到后丢弃前缀并继续组帧，找不到仍丢弃。
- UVC payload header error bit 对 MJPEG isoc 不再立刻 hard-skip，而是标记当前帧为 suspect，并继续拼到 EOF。
- EOF 时只有通过 SOI/EOI 检查，且没有 USB packet status error、invalid payload header、buffer overflow/underflow 等硬错误，才调用 `frame_cb`。
- 缺失 EOI 的 MJPEG 帧仍然丢弃。
- 修正 SoF payload 长度小于 2 时访问 `payload_data[0/1]` 的边界风险。

## 诊断日志

诊断开启时会输出累计统计：

```text
diag totals: xfers=... pkts=... payload_pkts=... bytes=... sof=... eof=...
delivered=... suspect_delivered=... suspect_dropped=... recovered_soi=...
skipped=... skipped_complete=... invalid_hdr=... frame_err=...
invalid_soi=... incomplete=... usb_err=... underflow=... overflow=...
max_payload=... max_frame=...
```

异常样本会记录：

- USB packet status、actual byte count。
- UVC header length、raw header info、FID、EOF、error bit。
- parser 是否认为当前 packet 是 SoF。
- payload 前四个字节。
- payload 前 1024 字节内的 `soi_at` 和 `eoi_at`。
- EOF frame 的首尾字节、SOI/EOI 位置和完整性判断。

## 实机验证

诊断构建和烧录命令：

```powershell
idf.py -DTAB5_DIAG_FACTORY_AUTO_START=ON -DTAB5_DIAG_FACTORY_MODE_INDEX=4 -DTAB5_DIAG_UVC_ISOC=ON build
idf.py -p COM35 flash
```

最终实机串口日志：

```text
D:\work\myself\otool_tab5_uvc_monitor\uvc-isoc_serial_fix2_20260701_110455.log
```

关键启动日志：

```text
Factory mode selected: index=4 MJPEG(HW) 1920x1080 @15.0 fps
Opening stream: addr=1 stream=0 fmt=MJPEG(HW) 1920x1080 @15.0 frame_buffers=3 frame_size=auto(0) urbs=3 urb_size=16384
Stream started: MJPEG(HW) 1920x1080 @15.0 fps
```

最终统计：

```text
diag totals: xfers=90961 pkts=545766 payload_pkts=127609 bytes=168481336 sof=1050 eof=1049 delivered=1034 suspect_delivered=62 suspect_dropped=15 recovered_soi=0 skipped=0 skipped_complete=0 invalid_hdr=0 frame_err=3305 invalid_soi=6 incomplete=15 usb_err=0 underflow=0 overflow=0 max_payload=3048 max_frame=167498
```

相对首个统计点的 69 秒增量：

```text
eof +1034
delivered +1019
suspect_delivered +62
suspect_dropped +15
skipped +0
skipped_complete +0
invalid_hdr +0
usb_err +0
underflow +0
overflow +0
incomplete +15
```

显示链路仍约 5.45 fps：

```text
display stats: 5.47 fps frames=55/10050ms avg_ms wait=0.43 decode=28.41 convert=0.00 ppa=96.08 blit=56.35 total=181.33 mode=MJPEG(HW) 1920x1080
```

实机结论：

- `skipped_complete=0`：完整 MJPEG 不再仅因为 UVC error bit 被丢弃。
- `suspect_delivered=62`：error-bit 帧中确实存在可交付的完整 MJPEG。
- `suspect_dropped=15`、`incomplete=15`：缺失 EOI 或无法完成的 suspect 帧仍被丢弃。
- `usb_err=0`、`invalid_hdr=0`、`underflow=0`、`overflow=0`：本轮数据不指向 USB status、payload header 格式或本地缓冲区压力问题。
- `recovered_soi=0`：当前相机主要收益来自 suspect-but-complete 策略，不是 SOI 前缀修剪；但保守恢复逻辑已经具备。
- 仍出现 18 次 `JPEG decode err`：SOI/EOI 只能证明字节边界完整，不能证明 JPEG entropy 一定可解码。上层 decoder 仍需要保持容错。如果产品要求完全消除 warning，需要在上层统计并丢弃解码失败帧，或引入更重的 JPEG 校验。

## 2026-07-01 后续复现补充

用户再次提供的附件日志中，文件名指向 `1280x720 @30`，但实际开始的视频流是用户确认后的 mode 4：

```text
Stream started: MJPEG(HW) 1920x1080 @15.0 fps
uvc-isoc: usb err #1 pkt=2 status=1 actual=0 requested=3060 transfer=0
uvc-isoc: invalid UVC payload header #1: 95 d0 len=64 status=0
```

这和前一轮“仅 UVC error bit 置位但 USB packet status 正常”的情况不同。`usb err` 表示 isochronous packet 本身已经有底层传输错误，当前代码继续 hard-skip 是正确的；否则会把真实缺包后的 JPEG 交给上层。

随后在目标设备上补做了两组诊断采集：

- `uvc-isoc_serial_usb_err_repro_20260701_113739.log`：mode 4，`MJPEG(HW) 1920x1080 @15.0`。
- `uvc-isoc_serial_1280x720_30_20260701_114134.log`：mode 0，`MJPEG(HW) 1280x720 @30.0`。

1920x1080@15 最终统计：

```text
diag totals: ... sof=707 eof=707 delivered=688 suspect_delivered=35 suspect_dropped=19 recovered_soi=0 skipped=0 skipped_complete=0 invalid_hdr=2 frame_err=2328 invalid_soi=3 incomplete=19 usb_err=4 underflow=0 overflow=0 max_payload=3048 max_frame=195718
```

现象：

- 约 15.6 秒处出现一次短时 USB isoc packet error 爆发，`usb_err=4`，同时 `invalid_hdr=2`。
- 之后到采集结束 `usb_err` 和 `invalid_hdr` 没有继续增长。
- `suspect_delivered=35`，说明 UVC error bit 噪声仍有部分可恢复。
- `JPEG decode err` 仍出现 19 次，说明 SOI/EOI 完整不等于 JPEG 内容一定能被硬件 decoder 接受。

1280x720@30 最终统计：

```text
diag totals: ... sof=1381 eof=1381 delivered=1350 suspect_delivered=36 suspect_dropped=31 recovered_soi=0 skipped=0 skipped_complete=0 invalid_hdr=0 frame_err=632 invalid_soi=6 incomplete=31 usb_err=0 underflow=0 overflow=0 max_payload=3048 max_frame=41940
```

现象：

- 这轮没有复现 `usb_err`，也没有 `invalid UVC payload header`。
- 仍有少量 incomplete 和 `JPEG decode err`，更像相机 MJPEG/UVC error bit 噪声或 JPEG 内容级别问题，不是 USB packet status 层持续丢包。
- 显示统计约 `8.8 fps`，decode 约 `13.5 ms`，PPA 约 `44.7 ms`，blit 约 `54.5 ms`。

补充结论：

- 用户附件中的 `usb err status=1 actual=0` 属于真实传输层错误，不应该被 `suspect-but-complete` 逻辑放过。
- 1280x720@30 在本次目标设备采集中没有 USB 层错误，因此更适合作为默认可选模式。
- 如果后续仍要改善 1920x1080@15 的偶发 `usb_err`，方向应是 transport sizing 或模式策略，例如测试更多 URB、不同 URB size、降低模式，而不是放宽 MJPEG 完整性检查。
- 如果主要问题是 `JPEG decode err` 日志噪声，则应在上层把“解码失败即丢帧”作为正常容错路径，并考虑限频统计日志；驱动层仅用 SOI/EOI 无法完全预测硬件 JPEG decoder 是否接受该帧。

## 2026-07-01 1080p 显示黑边补充

用户在默认界面手动选择 `MJPEG(HW) 1920x1080 @15.0` 后，观察到左下角有明显黑边。该现象不是 `uvc-isoc` 组帧问题，而是显示链路中 PPA SRM 缩放量化导致的铺屏误差。

根因：

- 物理屏是 `720x1280`，1080p 横屏旋转 90 度后要铺满竖屏，理想缩放比例是 `720 / 1080 = 1280 / 1920 = 2/3`。
- 旧日志打印的是浮点估算：`scale=0.667 out=720x1280`。
- 但 ESP-IDF PPA SRM 驱动实际把 scale 拆成 `int + frag/16`，`2/3` 会向下量化为 `10/16 = 0.625`，真实输出约为 `675x1200`，因此会在屏幕边缘留下黑边。

修复：

- 修改 `main/display_pipeline.cpp` 的 `ppa_scale_rotate()`。
- 对“本来应铺满屏幕”的模式改用 cover 策略：选择能覆盖目标尺寸的下一个 PPA 量化比例，并从源图中心裁剪。
- 对 1920x1080 -> 720x1280，本次实机使用 `q=11/16`，源图中心裁剪到 `1863x1048+28,16`，PPA 实际输出 `720x1280`。
- 日志新增 `crop=...` 和 `q=N/16`，后续可以直接从串口判断显示尺寸是否按 PPA 实际量化值计算。

实机验证：

```text
Factory mode selected: index=4 MJPEG(HW) 1920x1080 @15.0 fps
Stream started: MJPEG(HW) 1920x1080 @15.0 fps
display scale: src=1920x1080 crop=1863x1048+28,16 phys=720x1280 avail_h=1280 out=720x1280 off=0,0 scale=0.688 q=11/16 adv=0
```

结论：串口侧已验证 1080p 的 PPA 输出尺寸从旧行为的隐含约 `675x1200` 修正为实际 `720x1280`。这应消除由 PPA 缩放量化造成的黑边；若仍观察到边缘异常，应再区分是物理面板可视区、外壳遮挡、相机画面本身黑边，还是后续 overlay/flush 覆盖问题。

## host_test 状态

已补充 host test 代码覆盖以下场景：

- isoc 中间 payload packet 设置 UVC error bit，但最终 MJPEG 有完整 SOI/EOI，应交付。
- SoF payload 前面有垃圾字节，后面出现 `FFD8`，应修剪前缀并交付。
- 缺失 EOI 的 MJPEG 帧应丢弃，下一帧有效帧应恢复。
- SoF payload 长度为 0 或 1 时应拒绝，不崩溃且不调用 frame callback。
- shared scenario 按 transport 区分：bulk 仍丢弃 UVC error bit 帧，isoc 允许完整 MJPEG suspect 帧交付。

说明：host_test 是为了给后续 CI 或代码回归提供保护，不是本轮目标设备验收的必要条件。本轮验收以目标设备烧录后的串口日志和显示行为为准。

已尝试：

- 直接构建 `components/usb_host_uvc/host_test`：失败，原因是该 host_test 需要 Espressif 原始仓库布局中的 sibling `usb` 组件。
- 临时创建 `D:\work\myself\usb -> D:\work\example_project\esp-usb\host\usb` junction 后，依赖路径问题解决，但 Windows 下 ESP-IDF linux target 的 clang 链接最小 C 程序失败，`ld.lld` 不接受 `--major-image-version/--minor-image-version`，并且找不到 Windows 系统库。
- 增加 `-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY` 后，CMake 能进入 ninja，但 IDF linux 组件编译失败，缺少 `bsd/sys/cdefs.h` 和 `bsd/string.h`，CMake 同时提示缺少 LIBBSD。
- 再临时添加本地 shim header 试探后，错误继续深入到 Windows/ESP-IDF linux target 不兼容层面：缺少 `endian.h`、未知 `_LOCK_RECURSIVE_T`、`fls` 声明冲突。

因此，当前不再把 host_test 作为本轮阻塞项。若后续需要把这些用例纳入自动化回归，再迁移到 Linux、WSL，或 Espressif 支持的 linux target host-test 环境，并安装 `libbsd-dev` 等依赖后执行。

## 生产构建状态

生产配置已验证可关闭诊断：

```powershell
idf.py -DTAB5_DIAG_FACTORY_AUTO_START=OFF -DTAB5_DIAG_FACTORY_MODE_INDEX=-1 -DTAB5_DIAG_UVC_ISOC=OFF build
```

该构建通过后，默认 `CONFIG_UVC_ISOC_DIAG=n`，不会输出诊断统计日志。

## 风险与后续建议

保留风险：

- SOI/EOI 完整不等于 JPEG 内容一定可解码，实机仍观察到少量 `JPEG decode err`。
- 当前 1920x1080@15 MJPEG(HW) 的显示链路仍约 5.45 fps，瓶颈主要在 decode/PPA/blit，不属于 `uvc-isoc` 重组逻辑本身。
- host_test 代码已经补齐，但属于可选回归验证；当前目标设备验收不依赖它。

建议：

- 保持 1920x1080@15 MJPEG(HW) 为 not recommended 模式。
- 保留 `CONFIG_UVC_ISOC_DIAG`，用于后续相机适配和问题复现，但默认关闭。
- 若要继续降低 `JPEG decode err`，优先在上层记录“解码失败即丢帧”的统计，确认是否影响显示体验。
- 若未来实机出现 `usb_err`、`invalid_hdr`、`underflow` 或 `overflow` 增长，再考虑调整 URB size/count、frame buffer 数量或降低分辨率/帧率。

## 验收清单

- [x] 没有 SOI 和 EOI 的 MJPEG 帧不会到达 `frame_cb`。
- [x] UVC error bit 但最终 SOI/EOI 完整的 isoc MJPEG 帧可以交付。
- [x] 缺 EOI 的 MJPEG 帧继续丢弃。
- [x] SoF payload 长度 0/1 的边界场景不会越界读。
- [x] 实机日志显示 `delivered=1034/1049`、`suspect_delivered=62`、`skipped_complete=0`、`incomplete=15`。
- [x] 生产构建可关闭诊断，`TAB5_DIAG_UVC_ISOC=OFF` 构建已通过。
- [x] host_test 不作为本轮目标设备验收项；相关用例已补齐，后续可在 Linux/WSL/libbsd 环境纳入回归。

## 当前结论

不建议直接放过 `uvc-isoc` 错误。本次实现采用的是更窄的恢复策略：只对 USB packet status 正常、payload header 可解析、但 UVC error bit 置位的 MJPEG isoc 帧做 suspect 标记，继续组帧，并在 EOF 使用 SOI/EOI 和硬错误状态做最终判定。

对这台相机和该模式而言，修复后实机证据显示该策略能恢复部分完整帧，同时仍能丢弃缺失 EOI 的帧。本轮以目标设备实测为验收依据，不需要额外把摄像头接入本地系统；只有在需要 PC 侧抓包、协议复现或 CI 回归时，才需要另行准备本地/WSL/Linux 测试环境。
