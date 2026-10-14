# GL / Skia Vulkan 自动亮度与 10 位输出测试

应用默认进程模式不变。GL 自动读取系统亮度，无需增加配置；手机版独立 Skia
Vulkan 继续使用已有实验开关：

```ts
config.experimentalPhoneFork = true;
config.gpuProcess = 'separate';
config.skiaBackend = 'vulkan';
config.angleBackend = 'gles';
// vulkanOutputMode 默认 auto，无需填写两个亮度参数。
```

外壳使用新 HAR 中的 WebWindow / NativeContext，即自动接入
`display.getBrightnessInfo()` 和 `brightnessInfoChange`（API 22）。直接调用
C++ NAPI 而绕过 NativeContext 的外壳，需要调用 `UpdateDisplayBrightness`，
参数依次是 displayId、sdrNits、currentHeadroom、maxHeadroom，并转发变化事件。

`auto` 默认尝试 10 位：屏幕支持 HDR10 且取得亮度时使用 HDR10/PQ；否则尝试
SDR10。实际输出目标失败会记录原因并回退 SDR8。透明独立窗口仍为 SDR8。
10 位描述缓冲区精度，Skia Vulkan 名称本身不能证明实际输出已是 10 位。

## 怎么测

安装新 HAR 打出的应用，冷启动后正常浏览、播放已有 HDR 内容，并调节系统亮度，
观察白底、暗部、高光及前后台切换。无需手填 203/1000，也无需逐组切换参数。
如果画面异常，保存当次日志后使用以下配置并完整重启：

```ts
config.vulkanOutputMode = 'sdr8';
```

该开关只关闭 Vulkan 的 10 位/HDR 输出。若 Vulkan 初始化本身仍崩溃，可把
`skiaBackend` 改回 `'gl'` 后重启；独立 GPU / renderer 配置不必改成单进程。

## 日志判断

- `OHOS brightness: display=... white_nits=... peak_nits=... max_headroom=...`：
  已取得系统亮度。由系统事件驱动更新，不逐帧查询系统服务。
- `OHOS Vulkan: creating instance; requested_layers=0 swapchain_colorspace=...`：
  Skia Vulkan 正在初始化。后面的 instance initialized 才表示创建成功。
- `OHOSVulkan` 标签下的 `Dawn vkCreateInstance begin`、`requested layer`、
  `available layer`、`succeeded` 区分 WebGPU 创建阶段；可用于定位 layer 加载崩溃。
- 手机 `OHOS phone Vulkan: negotiated output mode=... native_format=...`，以及
  `OHOS phone Vulkan: active mode=...`：先协商，再确认实际提交。
  mode 0/1/2 分别表示 SDR8/SDR10/HDR10；native_format 12/34 表示 RGBA8/RGB10A2。
- `HDR luminance updated white_millinits=... peak_millinits=...`：Vulkan 已收到
  与该帧色彩转换一致的更新。这里单位是毫尼特；除以 1000 得到 nits。
- GL 保留原有 reshape / 色彩空间日志，亮度更新沿 DisplayColorSpaces 生效。
- 平板/PC WSI 日志包含实际 swapchain format、color_space、output_mode。

外壳的 renderer JS 回应、配置开关为 true、屏幕支持 HDR，都不能单独证明 Vulkan
已经工作。普通截图可以辅助发现偏色，通常经过 SDR 转换，不用于证明面板位深。

## 已知边界

Vulkan 输出模式在窗口初始化时选择，调亮度不会切换 SDR/HDR 格式或重启 GPU。
动态参数使用系统当前 headroom，最大 headroom 只作为上限；headroom=1 是合法的
临时状态。可选 `vulkanHdrWhiteNits` / `vulkanHdrPeakNits` 仅用于固定值调试，
正常使用时应删除，避免覆盖自动更新。

目前 Vulkan HDR 限单显示器；外接屏变化可能触发 SDR 回退。Vulkan 每个输出单独
协商色彩，浏览器全局 CSS HDR 声明仍保守。视频 Vulkan 零拷贝留到此轮输出完成后。

68e8b50 真机报告已确认发生 Vulkan layer 加载阶段 SIGBUS，不能据此判断面板不支持
10 位。本轮修正 Skia 的扩展误传 layer 参数，并保留 fork 后链接器需要的 RELRO 缓存 FD。
现存完整 crash 确认 Dawn 创建 instance 时加载 Trace3D，RELRO 写入目标却映射了
Chromium 共享内存，详见 [诊断记录](OHOS_VULKAN_RELRO_FD_DIAGNOSIS.md)。
新包先看 instance 成功及实际提交日志；修复是否消除 SIGBUS，以实测为准。
如果再次崩溃，请同时保存完整 native crash（含 maps / OpenFiles），避免只有 hilog 摘录。
