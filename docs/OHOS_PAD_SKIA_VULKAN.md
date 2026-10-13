# Pad / PC 常规 Skia Vulkan 输出颜色协商

## 2026-10-14：6963231f 现场

Pad 使用 `--use-vulkan=native --use-angle=vulkan`，没有 `--ohos-phone-fork`。
GPU 能创建 Vulkan surface，但首次 Reshape 请求 `RGBA_1010102`，Vulkan 输出
能力表只有 8 位格式，`color_type=0` 触发断言。连续三个 GPU 退出后回退
GaneshGL；ANGLE Vulkan 仍运行，不能把它作为 Skia Vulkan 成功的依据。

现场还显示 GL 回退没有可用的 10 位 EGL window config，但上层仍请求 10 位。
因此仅向能力表添加 10 位格式会掩盖真实的不匹配。

## 本次修复

核对构建机实际源码：`SkiaOutputDeviceVulkan` 使用 `FORMAT_RGBA_32` 创建
swapchain，`VulkanSurface` 只选择 RGBA8/BGRA8 UNORM 和 sRGB nonlinear。
面板报告支持 HDR，不代表这条呈现路径支持 10 位 / PQ 输出。

- OHOS 启动时请求原生 Skia Vulkan，屏幕统一声明 RGBA8 sRGB，HDR 内容由
  Chromium 转为 SDR。手机 fork 原有的 SDR 限制继续有效。
- 浏览器启动每一代 GPU 时传入内部 `--ohos-sdr-output` 标记。GPU 的实际
  Vulkan 设置即使因失败而被禁用，这个输出约定也保留；该标记不启用 Vulkan。
- GL 回退使用相同的 SDR 策略，禁止选择 10 位 EGL config，NativeWindow 请求
  RGBA8，并在新建或替换窗口时明确设置 sRGB / 无 HDR 元数据。
- 常规 Vulkan 补齐 RGBX 的 8 位映射，保留 alpha 语义；打印实际 swapchain
  VkFormat、Vulkan color space 和 SkColorType。保留未知格式断言，不伪造 10 位支持。

此限制按 **Skia Vulkan 会话** 生效。仅开启 ANGLE Vulkan、Skia 仍为 GL 的
会话不会因此改变原有 HDR 策略；GL-only 的一般性 EGL 能力动态协商不在本修复范围。
本次不增加 10 位 Vulkan / HDR 呈现支持；这需要同时实现并验证 swapchain 格式、
SkColorType、颜色空间、NativeWindow 元数据以及失败后的能力回传。

## 真机验收

替换新 HAR，确认内核提交号后完整退出并重启应用。

| 配置 / 操作 | 预期 |
|---|---|
| Pad 独立 GPU + Skia Vulkan + ANGLE GLES | GaneshVulkan，GPU 不因格式协商退出 |
| Pad 独立 GPU + Skia Vulkan + ANGLE Vulkan | 同上；分别核对两种后端 |
| SDR 网页、透明弹窗、窗口缩放 / 旋转 | 颜色、透明度正确，没有黑帧或花屏 |
| HDR 视频打开 / 关闭、标签切换 | 内容转 SDR；根输出保持 8 位 sRGB |
| 同一会话受控触发 GPU 失败并回退 GL | 输出仍为 8 位 sRGB，无 10 位请求 / 8 位窗口组合 |
| 关闭 Skia Vulkan 后冷启动 | 恢复 GL 会话原有策略，不遗留内部 SDR 标记 |

保留以下日志及 `chrome://gpu`：

```text
OHOS display: session output is RGBA8 sRGB; HDR content is tone-mapped
OHOS Vulkan: swapchain format=... color_space=... sk_color_type=...; output is 8-bit sRGB
OHOS Skia reshape: format=RGBA_8888 ... color_space=... mappings=[...]
```

`In-process GPU: false` 和 `GaneshVulkan` 应同时出现。初始化前若已回退，可能没有
swapchain 日志；应保留完整启动日志排查。CI 构建通过不代表真机画面验收通过。
