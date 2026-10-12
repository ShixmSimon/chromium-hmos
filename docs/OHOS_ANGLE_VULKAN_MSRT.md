# OHOS ANGLE Vulkan 多重采样兼容处理

2026-10-13。状态：已实现保守兼容补丁，设备回归待完成。

## 报告与已确认的源码路径

外壳报告 `2026-10-13-pad-angle-vulkan-corruption.md`：GEM-W00，系统
`7.0.0.109(SP6C00E105R3P3)`，HAR `77a3cb41`。开启 ANGLE Vulkan 后，Chrome
应用商店首页最大化时网页内容花屏，ArkUI 正常；仅关闭 ANGLE Vulkan 并重启后恢复，
WebGPU 保持开启。该对照尚未独立核实 renderer，也未做重新开启的 A/B/A。

通过 CI 只读快照确认实际 ANGLE revision 为
`e12217f3e133cb1029b050d893b1806d141483be`，Chromium revision 为
`b510e9d7cd3a2fbd78d0ddc42234103206c5f78d`。

源码证据：

- `vk_renderer.cpp::CanSupportMSRTSSForRGBA8` 只探测基本 2D RGBA8 和 sRGB RGBA8
  的特定 usage/flags 组合，通过后可启用 `supportsMultisampledRenderToSingleSampled`。
- `TextureVk.cpp::initImage` 第 4373 行与报告日志一致。实际纹理已绑定 MSRTT
  framebuffer，但原格式或线性/sRGB 附加视图格式的查询失败，于是主动返回
  `VK_ERROR_FORMAT_NOT_SUPPORTED`。此分支要求 `mOwnsImage`，不能由这行日志直接认定
  是 OH_NativeBuffer 导入失败。
- `ImageHelper::FormatSupportsUsage(RequireMultisampling)` 要求底层查询成功且
  sampleCounts 包含多采样能力。因此日志的 `-11` 不一定是驱动原样返回的结果；也可能
  驱动查询成功但仅报告单采样。失败的格式、usage、flags 和 GPU/驱动尚未从设备采集。
- `TextureVk::getAttachmentRenderTarget` 在 MSRTSS 关闭且 MSRTT 扩展可用时，已有
  独立多采样附件和 resolve 实现。若设备不满足上游 emulation 条件，ANGLE 不暴露
  `GL_EXT_multisampled_render_to_texture{,2}`，Skia 的 `GrGLCaps::initFSAASupport`
  可选择常规 GLES 3 多采样 FBO 路径。

这些证据确认了报错路径，尚不能确定具体不兼容的格式组合，也不能宣称所有花屏均由
该分支导致。没有证据指向防钓鱼代码或窗口 stride。

## 补丁行为

`patches/ohos-angle-vulkan-msrt.patch` 在 OHOS 的 ANGLE Vulkan renderer 初始化时，
默认关闭 MSRTSS，且在计算所有依赖特性和启用 Vulkan device extensions 之前处理。
保留上游的 MSRTT emulation 条件：允许时走独立多采样附件，否则不暴露 MSRTT 扩展。
不强行启用驱动尚未通过能力判断的 emulation，不在失败后忽略绘制错误继续提交。

范围是整个 OHOS ANGLE Vulkan 后端：报告没有 GPU/驱动标识，不能安全地编写特定 GPU
黑名单。ANGLE Vulkan 仍是实验选项，默认 GLES；WebGPU、直接 Skia Vulkan 和 GLES
后端不受该补丁影响。MSRTSS 关闭可能增加显存、resolve 开销，并减少依赖它的 multiview
扩展能力，需要设备评估。显式 ANGLE feature override 仍遵循上游约定，供内核诊断使用；
正常外壳无需新增参数或接口。

诊断：

- 每次 renderer 初始化输出一行 `OHOS ANGLE Vulkan MSRT:`，包含 device、vendor、
  deviceId、原始 driverVersion/apiVersion，以及最终 nativeMSRTSS、emulateMSRTT、
  exposeMSRTT 状态。正常修复包应为 `nativeMSRTSS=0`。
- MSRTSS 格式查询不满足要求时输出 `OHOS ANGLE Vulkan MSRT query rejected:`，包含
  format、type、tiling、usage、flags、底层 result、sampleCounts。所有数值为 Vulkan
  原始枚举/位掩码。sampleCounts 是支持集合，不是该次渲染请求的采样数；失败查询的
  sampleCounts 不用于判断能力。
- 新增查询日志在同一进程中最多 8 条，跨线程共享额度。原有 ANGLE GL error 日志仍由
  上游处理。常规配置避开失败路径；强制重新启用 MSRTSS 的诊断配置可能再次花屏。

## 构建和验证

全新应用：在 `patches/deps/third_party_angle.patch` 后，对 ANGLE 子仓库应用本补丁。
增量构建：`apply_incremental_patch` 接受可选仓库路径，支持该 DEPS 子仓库；默认仍为
Chromium。通过自动测试检查两种路径的首次应用、重复应用、旧版回退升级，以及子仓库
补丁不会误写 Chromium 同名文件。

本地验证从固定 ANGLE revision 下载相关原文件，应用基线 ANGLE 补丁后与 runner 快照
逐字节一致，再验证新补丁正向应用和反向检测。原生编译和 HAR 打包由 CI 完成。

## 外壳侧验收（待设备执行）

1. 更换修复 HAR 并完整重启；记录 HAR 提交、系统版本和 `chrome://gpu` renderer。
   先保持 ANGLE Vulkan 关闭、WebGPU 开启，确认 GLES 对照正常。
2. 开启 ANGLE Vulkan、保持 Skia GL 和相同其他设置，完整重启；确认 renderer 为 ANGLE
   Vulkan，并保存新的 MSRT 诊断行及 GPU/驱动信息。检查无该组反复 TextureVk/-11 错误。
3. 在相同商店首页做普通窗口、最大化、恢复、连续调整尺寸和滚动；检查文字、图片、
   圆角、阴影、透明图层以及深浅色。切换标签和重载后重复。
4. 检查 Canvas 2D、WebGL/WebGL2 抗锯齿、视频播放及硬件解码页面；记录内存、帧率和
   GPU context lost。如 WebGPU 原来可用，确认其仍可用。
5. 再关闭 ANGLE Vulkan 并重启，完成修复包的 GLES/Vulkan/GLES 对照。若仍花屏，保留
   新日志和 `chrome://gpu`，继续关闭实验开关使用 GLES；不能把“旧错误消失”单独当作
   画面修复通过。

本轮未修改 Lumi 外壳、未连接或操作设备。最终结论应区分“CI 编译通过”和“设备回归通过”。
