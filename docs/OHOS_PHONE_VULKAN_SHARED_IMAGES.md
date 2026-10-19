# 手机 Skia Vulkan 纹理共享兼容修复

2026-10-14，针对 `7e55063` 已能显示 SDR10 首帧、但视频 / Canvas / WebGL
创建 SharedImage 失败的现场。YouTube 证据记录 126 次创建失败、108 次引用不存在
mailbox 的错误，同期没有 GPU 重启或 Vulkan 回退。尚未证明每次闪烁都由这些错误造成。

## 实现

- 在 OHOS 编译 Chromium 154 原有 `ExternalVkImageBackingFactory` 及同步依赖；
  只在 `IsOhosPhoneGpuChild()` 且 Ganesh Vulkan 时注册并创建信号量池。
- 保留手机 GPU 的通用 NativePixmap 限制。RGBA WebGL / Canvas 可使用 ExternalVk；
  shared-memory NV12 视频由 CompoundImageBacking 的共享内存和 GPU backing 配合上传。
- 当前手机 GL 缺少 external-memory / semaphore 扩展，上游实现会使用独立 GL / Vulkan
  纹理，在内容跨 API 使用时读回和上传。不是纹理端的全程零拷贝，也不是最终画面
  回退软件绘制；屏幕输出继续使用原有 shared NativeBuffer 路径。
- OHOS ExternalVk 没有 Dawn representation，这个会话通过
  `disable_webgpu_shared_images` 上报限制；视频池遵循该能力，不再仅因
  `enable-unsafe-webgpu` 就请求可选的 `WEBGPU_READ`。这不等同于关闭整个 WebGPU，
  但也不承诺本轮修复 WebGPU canvas / 视频直接导入。
- 单进程、手机 GL、平板和 PC 不注册这个新 factory。没有改动 HDR 选择和亮度策略。

## 外壳复测

替换 HAR，确认 `getAboutInfo` 提交号，保留已验证配置后完整退出并冷启动：

```ts
config.experimentalPhoneFork = true;
config.gpuProcess = 'separate';
config.angleBackend = 'gles';
config.skiaBackend = 'vulkan';
```

1. 确认 `GaneshVulkan`、独立 GPU，以及 SDR10 首帧日志。新增日志应包含
   `OHOS phone Vulkan SharedImage bridge enabled; external_object_interop=0`。
   它仅证明工厂注册；仍需下面的实际画面和错误日志验收。
2. 同一 YouTube 视频测试播放、暂停、拖动、全屏、滚动和标签切换，再测现有 Canvas /
   WebGL 页面。首轮保持氛围模式等网站设置不变，避免混入其他变量。
3. 收集从冷启动至结束的日志和闪烁时间点，检查 `Could not find SharedImageBackingFactory`、
   `Unable to create shared image`、`non-existent mailbox`，同时检查复制、上传、
   representation 和 GPU 退出错误。画面正确且这些失败消失，才算纹理兼容通过。
4. 对仍闪烁的时间点保留录屏 / trace；不能只因计数减少就判定根因已修复。
   同页面 GL 对照只改 `skiaBackend` 并冷启动。记录帧时间、视频 dropped frames、
   内存趋势；120 Hz 的显示预算为 **8.33 ms**。跨 API 复制可能带来额外开销。

本轮不要求重新验证 HDR 亮度。SDR10 输出包中的 white / peak 为 0 是该模式的正常字段，
不能由此推断系统亮度获取失败；HDR10 仍须以实际输出模式和元数据证明。

## 构建检查与后续

新补丁基于固定 Chromium 154 revision 及其前置补丁生成。检查干净应用、反向检查和
增量重放，完整目标编译由 Actions 执行。编译成功不代替上述手机纹理和性能验收。
后续直接 NativeBuffer 纹理互操作需要独立设计分配、导入和 fence 所有权；不在本轮放开
尚未实现的通用 NativePixmap 导入。
