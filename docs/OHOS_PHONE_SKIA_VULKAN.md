# 手机独立 GPU 的 Skia Vulkan 实验

范围：phone/foldable，显式开启 `experimentalPhoneFork`，GPU 独立进程。
默认仍为单进程，不启用手机单进程 Vulkan。平板/PC 继续原有官方子进程和
Vulkan swapchain 路径。本实验不切换 ANGLE Vulkan。

## 外壳配置

启动内核前设置，保存后必须完整退出并重启应用，不能只刷新网页：

```ts
config.experimentalPhoneFork = true;
config.gpuProcess = 'separate';
config.angleBackend = 'gles';
config.skiaBackend = 'vulkan';
```

保留现有已验证的 `jitless` 配置，不同时改变 JIT、WebGPU 或光栅化选项。
不要追加 `--single-process`、`--in-process-gpu`、`--disable-gpu`。
切回对照组只改 `skiaBackend = 'gl'` 并冷启动。

## 手机 fork 的系统媒体服务（2026-10-14）

手机 `experimentalPhoneFork + separate GPU` 模式通过 Chromium 原有 Mojo
MediaService 接口，将能力查询、原生视频/音频解码及 WisePlay CDM 放在**浏览器
进程的专用媒体线程**。独立 GPU 继续负责 GL/Vulkan 绘制。HTML 视频、WebCodecs
和帧级 DRM 使用同一服务；能力查询结果按服务生命周期缓存，避免重复查询。
单进程、浏览器内 GPU、官方 NCP 保留原有路由。

原因：Mate 70 Pro+ API 26 的 `0483a16` 在 GPU 查询音频能力时，经
`OH_AVCodec_GetCapability → HiAppEvent → FFRT HExecuteUnit::PokeAddRtq`
调用空函数指针而 SIGSEGV。系统 `libffrt.so` 已通过 `pthread_atfork` 注册
`ffrt_child_init`，不能靠重复调用内部初始化函数修复。只把能力查询搬回主进程
也不足以保证实际创建解码器正常，因此这次迁移整个播放服务。

fork 子进程仍拒绝直接使用 AVCodecKit，防止残留调用触发同一问题；这不再代表
播放器只能软件解码。浏览器服务根据真实系统能力提供 H.264/HEVC/VP9/AV1 和
AC3/DTS，未被系统报告支持的格式仍不能假报。硬件编码器尚未迁移，手机 fork GPU
中的原生编码路径仍关闭。

浏览器服务没有 GPU CommandBuffer，视频解码采用 AVCodecKit **Buffer 输出**，
通过 Mojo 共享内存传送 NV12/P010 及色彩/HDR 元数据；这是系统硬件解码，但包含
像素拷贝，不能称为视频零拷贝。原有 GPU Surface/SharedImage 路径不变，手机
Vulkan 最终 NativeBuffer 上屏仍沿用既有实现。

验收证据必须同时包含：

- `OHOS phone browser media: service bound outside fork GPU` 与
  `ready; video configs=... audio configs=...`，确认能力在浏览器线程读取。
- HEVC/AC3/DTS 实际媒体播放、拖进度、切换和销毁成功；不能只看
  `isConfigSupported()` 或 `canPlayType()`。记录解码器创建及实际输出日志。
- 10 位 HEVC 输出 P010，HDR 视频进入 HDR10，退出后回到 SDR10。
- 连续能力查询、多次冷启动和前后台切换，无 FFRT 崩溃、GPU 回退或无响应。
- 单进程和官方 NCP 回归：不出现 `phone browser media` 路由日志。

代码和构建通过不代表上述真机验收已通过。硬件编码、视频零拷贝和长期稳定性
分别验证，不能用软件 VP9 HDR 样片替代 HEVC 硬解验收。

## 如何确认实际启用

同时满足：

- `getAboutInfo` 的内核提交号对应本次构建。
- `chrome://gpu`：`In-process GPU: false`、Skia renderer 为 `GaneshVulkan`。
- 日志包含 `OHOS phone Vulkan: first shared NativeBuffer frame presented`。

只有 `--use-vulkan=native`、Dawn Vulkan adapter 可用或独立 GPU PID，均不算通过。
若显示 GaneshGL，检查 `OHOS phone Vulkan:` 和 `OHOS Vulkan:` 日志，这是回退，
不能计为 Vulkan 测试成功。

## 验收顺序

1. 单进程 GL 基线：确认本次包正常启动、输入、网页和视频。
2. 实验多进程 + separate + GL：与上一版作回归对照。
3. 实验多进程 + separate + Vulkan：确认上述三个启用证据，再测滚动、Canvas、
   WebGL、视频、透明内容、标签切换、前后台、旋转和窗口尺寸变化。检查倒置、颜色、
   残影、黑帧和白屏；至少 20 轮标签开关、3 次多标签冷启动。
4. 在 Vulkan 已上屏后使用已有受控 GPU 崩溃入口：新 GPU 应在本会话回退到 GL，
   网页可恢复，日志说明 `unavailable or failed earlier; use GL`。完整退出重启后
   才重新尝试 Vulkan。回退仍失败或循环重启即为不通过。
5. 同页面、温度、电源模式比较 GL/Vulkan：120 Hz 的帧预算为 **8.33 ms**，记录实际
   显示帧率、帧间隔 P50/P95/P99、超过预算比例、FD 和内存趋势。区分队列等待、
   GPU 执行与提交；提交次数和提交 ACK 不是实际上屏帧率。

失败时保留 GPU 报告、完整启动至故障日志、截图/录屏、trace 和配置。
回退是容错成功，不代表 Vulkan 功能通过。长期稳定性由真机验收。

## 实现与边界

主进程 RequestBuffer，传 NativeBuffer 元数据/FD 和 acquire fence；子进程通过
`VK_OHOS_external_memory` 导入 RGBA VkImage，Skia 直接绘制。绘制前等待 acquire
semaphore，提交时释放到 GENERAL / FOREIGN queue family，再导出 SYNC_FD completion
fence，由主进程 FlushBuffer。Vulkan 成功导出时的 -1 表示已完成，不是导出失败。
成功输出路径不读回像素、不 CPU 拷贝、不做最终整帧 blit；不代表网页所有渲染阶段
都没有拷贝，WebGL/视频的跨 API 共享仍须单独测试。

缓存最多 8 个导入；每次使用不同 lease 时全帧重绘，不依赖内容保留。正常帧使用 GPU
同步；resize/缓存清理/销毁等待已提交绘制完成后释放 VkImage 和 NativeBuffer。
窗口暂未注册或尺寸未对齐时临时绘制后丢帧，下帧重新申请，不把它误判为 Vulkan 故障。
不支持导入、同步错误或呈现错误时记录阶段并退出 GPU，由现有 GPU 恢复接管。
导入超时 2 秒终止子进程；浏览器将未确认完成的 lease detach，并锁定本会话 GL 回退。
任何尝试过 Vulkan 的 GPU 代际结束均采取同样的保守回退策略。

实际显示时间仍未知，反馈使用提交确认，刷新间隔从主进程 NativeVSync 获取。
私有 Parcel 布局、驱动 NativeBuffer 导入和跨 API 资源兼容性仍需真机确认。

## 2026-10-14：启动时颜色格式断言修复

`ae5f4ecb` 手机现场已符号化至 `SkiaOutputSurfaceImpl::Reshape()` 的
`CHECK_NE(color_type_, kUnknown_SkColorType)`。这发生在 output device 的
`Reshape()` 之前，GPU 重启后回退 GaneshGL，多进程仍开启。旧日志没有实际
`params.format`，因此不能断言该次请求一定是 RGBX。

源码确认两处能力不一致，本次分别修正：

- 手机 Vulkan 补齐 `RGBX_8888` 映射。RGBA/RGBX/BGRA/BGRX 请求统一对应真实的
  RGBA8 NativeBuffer/VkImage，Skia 颜色类型为 `kRGBA_8888_SkColorType`；
  `alpha_type` 独立保留，不透明请求仍为 opaque。
- 手机 fork 独立 GPU 的 GL/Vulkan 共享传输均只支持 RGBA8，屏幕层现在声明
  RGBA8 sRGB 输出，并将 HDR 内容色调映射到 SDR。之前按面板 HDR 能力声明
  不透明窗口为 `RGBA_1010102`，也可能触发缺失映射断言。手机单进程及
  平板/PC 原有屏幕能力声明不变；此实验路径暂不支持 HDR 输出。

OHOS 在首次 Reshape、格式或 alpha 类型变化、映射缺失时，在断言前输出：

```text
OHOS Skia reshape: format=... color_type=... alpha_type=... color_space=... size=... mappings=[...]
```

映射缺失时清除上一轮颜色类型，避免错误沿用旧格式；保留原断言。手机 Vulkan
设备还会记录支持的四种请求格式和 RGBA8 SDR 物理格式，并拒绝意外的 HDR
颜色空间或非 RGBA8 Skia 类型，通过现有 GPU 恢复机制回退。

真机复测需完整退出应用后冷启动，保留上述日志及 Vulkan 首帧日志，同时核对
`GaneshVulkan` 与 `In-process GPU: false`。测试不透明网页、透明内容、HDR 视频、
旋转及标签切换；HDR 视频预期显示为 SDR。CI 编译成功只证明构建通过，
不能代替设备上的 Vulkan 启用及画面正确性验收。

后续 Pad 现场确认了常规 Vulkan 的 10 位请求 / 8 位 swapchain 不一致，现已将
SDR 输出策略扩展到 OHOS 原生 Skia Vulkan 会话及其 GL 回退，详见
[Pad / PC 输出颜色协商](OHOS_PAD_SKIA_VULKAN.md)。
