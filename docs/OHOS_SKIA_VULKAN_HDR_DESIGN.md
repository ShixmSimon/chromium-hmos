# Skia Vulkan HDR 与 10 位输出设计

日期：2026-10-14。设计基线：`4ec4b24`。状态：输出首版已实现，编译与真机状态见交付记录；不代表实屏验收通过。

目标是让实际输出目标、Skia 格式、Chromium 色彩变换与系统显示约定一致。
按“10 位 SDR → HDR10 输出 → HDR 视频输入”推进，每步单独构建、验收。
手机优先，平板/PC 在各自呈现路径上分别验证，共用能力与回退策略。

## 首版落实与原方案调整

外壳操作及完整限制见 [测试说明](OHOS_SKIA_VULKAN_HDR_TESTING.md)。

首版将模式固定到输出窗口会话，使用输出级 `DisplayColorSpaces`，不改变全局
ScreenInfo/CSS HDR 声明。先校验实际分配/导入或 swapchain，再发布能力，首帧
日志单独确认实际提交；系统合成器和面板仍必须真机验收。

手机逐 NativeBuffer 写入颜色/静态元数据，平板/PC 要求 Vulkan PQ 格式对和
`VK_EXT_hdr_metadata`，没有该扩展不尝试用窗口标签代替。首版静态元数据描述
显式配置的合成输出范围，不复制任意视频的 mastering metadata，因此此轮无需
扩展 OutputSurfaceFrame 的逐视频元数据传输。参考白与峰值是实验参数。

`auto` 不按每段内容热切换。有合法亮度参数且显示器支持 HDR10 时尝试 HDR10，
否则尝试 SDR10；实际目标不支持会回退 SDR8。透明输出 SDR8。
HDR 首版只接受单显示器，并在绘制中节流检查显示器变化；外接显示器时允许
回退，不承诺无缝迁移。视频 10 位输入需单独验收，视频 Vulkan 零拷贝按用户
最新安排留到输出完成后。

## 范围与模式

保留手机多进程门禁：`experimentalPhoneFork=true`、`gpuProcess='separate'`、
`skiaBackend='vulkan'`；首轮 `angleBackend='gles'`。手机单进程 Vulkan 不在范围内。
平板/PC 继续官方子进程。普通单进程 GL 的既有策略不随此实验改变。

本次实现的启动配置（须使用本次构建 HAR）：

```ts
config.vulkanOutputMode = 'sdr8'; // 'sdr8' | 'sdr10' | 'hdr10' | 'auto'
```

实验初期默认 `sdr8`。指定模式表示尝试并报告实际结果，不跳过能力检查。
`auto` 只选择已验证能力；第一轮先使用显式模式便于对照。启动配置改变仍需完整重启。
实现完成后的内容 SDR/HDR 切换由内核处理，不要求每次播放视频重启应用。

| 模式 | 不透明根输出 | 颜色语义 | 必要条件 |
|---|---|---|---|
| sdr8 | RGBA8/BGRA8 | sRGB SDR | 当前基线 |
| sdr10 | RGB10A2/BGR10A2 | sRGB SDR | 真实 10 位目标、Skia 绘制和系统呈现通过 |
| hdr10 | 10 位 RGB + 2 位 alpha | BT.2020、PQ、HDR10 | 10 位输出、PQ 呈现、亮度策略、元数据路径均有效 |
| auto | 按启动参数与探测选择并固定 | SDR 或 HDR10 | 有效实验亮度参数、显示器和实际目标验证 |

10 位只代表颜色数值精度；不能据此认定 HDR、广色域或面板原生 10 位。
第一轮不承诺 HLG 原样输出、HDR Vivid、HDR10+、Dolby Vision 或透明 HDR 窗口。
HLG 内容如要支持，须经正确输入色彩转换到已支持的 PQ 输出，不直接改标签。

## 已核对的基础与缺口

- `ohos_screen.cc` 的 `OhosRequiresSdrOutput()` 当前按会话强制 SDR，面板 HDR
  查询只检查是否存在非零枚举，还没有精确匹配 HDR10/PQ、窗口目标或 GPU 能力。
- 手机 `SharedBufferLease::Acquire()` 固定请求 NativeBuffer 格式 12（RGBA8），
  `ValidSharedPacket()` 也固定校验该格式，手机 Vulkan importer 固定接受 RGBA8。
- 平板/PC 使用 `SkiaOutputDeviceVulkan` → `VulkanSurface` → swapchain。
  上游 154 只按 8 位/565 偏好选择 VkFormat，并强制 sRGB nonlinear；Reshape
  未接收用于切换输出模式的完整颜色约定。
- 视频解码器已有 P010、输入 ColorSpace 和 HDRMetadata 传递；但
  `OhosVideoFrameConverter::Initialize()` 与 `CreateSharedImageForOhosVideo()`
  明确限制 Skia GL，原生 Skia Vulkan 会退到 buffer mode。
  因此“视频保留 10 位”与“视频 Vulkan 零拷贝”要分别验收。
- 154 的 `OutputSurfaceFrame` / `gfx::FrameData` 没有完整 HDR10 静态元数据，
  只有显示 HDR headroom 等信息。需要新增/贯通合成输出元数据，不能假设视频
  metadata 已自然到达 Present。
- `DisplayColorSpaces::GetRasterAndCompositeColorSpace()` 会读取 `needs_alpha=true`
  的颜色条目；不能把该条目全局设置 SDR 来处理透明弹窗，否则可能影响 HDR 合成。

## 共用能力协商

拟增加 `OhosOutputColorCapabilities` 与 `OhosOutputColorState`，区分请求模式、
探测能力、已经提交生效的模式。状态包含：display/window ID、GPU generation、
color generation、请求/实际格式、gfx/Skia/Vulkan 色彩类型、alpha 语义、
SDR white、HDR headroom、元数据支持和失败原因。

HDR 能力取以下条件的交集：

1. 当前窗口所在显示器支持相应 HDR 格式与色彩空间。查询准确枚举，不再用
   “任意 HDR 格式非零”；不能始终查询 display 0 代替窗口实际显示器。
2. 目标窗口/系统合成路径支持对应 buffer 与 PQ 呈现。
3. 当前 Vulkan device 支持实际格式的 render attachment、采样和必要传输用法；
   外部内存导入、同步以及 Skia wrap/draw 成功。
4. Vulkan swapchain 路径支持相应 `(VkFormat, VkColorSpaceKHR)` 对，或手机
   NativeBuffer 路径有经过验证的颜色/元数据传递契约。
5. 能将已确认的输出能力反馈到 browser/Viz，控制 DisplayColorSpaces、
   HDR headroom、输出 Reshape 和失败后的重建。

保持 SDR 启动。在真实窗口和独立 GPU 就绪后探测，GPU 回报可用能力，浏览器/Viz
再协商目标模式。不要在手机 fork server 启动前初始化 Vulkan、EGL 或显示驱动线程。
分配器预热仍保持现有有限操作；Vulkan 探测放在子进程内。

当前屏幕能力是全局声明，新增能力必须按显示器及输出窗口取交集；某个主窗口探测
成功不能让其他未验证窗口也收到 10 位请求。能力发布与实际模式提交分开，真正
切换完成后才报告 active mode。模式失败要撤回能力并触发 Viz 重新 Reshape。

## 手机共享 NativeBuffer 路径

继续使用 browser RequestBuffer → 子进程 Vulkan 直接绘制 → completion fence →
browser FlushBuffer。成功路径保持无最终像素读回或整帧 blit。

- 新版本共享协议显式增加 format、color mode、color generation、能力协商和有界
  元数据字段。版本/消息长度显式校验，旧协议保留 SDR8 或拒绝不兼容握手，不混读。
- browser 根据已协商模式请求 RGBA_1010102。读取真实分配配置与 Vulkan native
  properties，校验尺寸、stride、size、format；不能因为 SET_FORMAT 返回成功就
  宣告分配成功，不能简单把旧的格式校验从 12 改成“任意格式”。
- 将驱动返回的实际 VkFormat 精确映射到匹配的 SkColorType。区分
  `VK_FORMAT_A2B10G10R10_UNORM_PACK32` / `A2R10G10B10_UNORM_PACK32`；用数值
  测试图验证位序及红蓝通道。Unsupported format 不伪装成 RGBA8。
- packed RGB10A2 仍为 4 bytes/pixel，但 stride、分配尺寸仍使用实际值校验。
  不放宽现有 FD 数量、消息长度、64 MiB 资源边界或未知 Parcel schema 拒绝策略。
- 导入缓存键纳入 allocation ID、实际格式、尺寸、color generation；SkSurface
  的颜色空间/alpha 属性改变也必须重建，不能只按 buffer sequence 复用。
- acquire semaphore、GENERAL/FOREIGN ownership transfer 和 completion SYNC_FD
  保留。共享内存探测可能受驱动查询缺陷影响，允许有界的真实导入验证，但失败即
  判定该模式不可用，不把未知查询结果当作成功。
- 对实际 leased buffer 设置颜色/静态元数据，并在同一队列序列提交；所有系统对象
  操作由其 owner 执行。优先逐 buffer 元数据，窗口默认值作为补充，避免旧队列
  中 SDR/PQ 帧因全局窗口标签变化被错误解释。若系统只支持窗口级变更，转换时
  必须证明旧帧已退出消费或重建窗口/队列，不能靠“已发送 Flush”代替完成。

## 平板/PC swapchain 路径

扩展 OHOS 的 VulkanSurface 配置，让 Initialize/Reshape 接收目标格式与颜色空间，
格式改变时也重建 swapchain，不能只处理尺寸改变。对其他平台保留旧默认行为。

- 探测并按需启用 `VK_EXT_swapchain_colorspace`；按驱动枚举的完整格式/色彩空间
  对选择 10 位 sRGB 或 `VK_COLOR_SPACE_HDR10_ST2084_EXT`。
- 不强写 `surface_format.colorSpace = SRGB_NONLINEAR`。只有枚举/实际创建通过的
  格式对可用；OHOS WSI 若未提供 HDR 对，首版保持 SDR，记录“不支持 WSI HDR”。
  不把现有 sRGB swapchain 直接贴 PQ 标签当成通用适配。
- Skia 格式表与真实 swapchain 对齐，正确处理 RGBX/BGRX、10 位类型与 alpha。
- 有 `VK_EXT_hdr_metadata` 时评估使用 `vkSetHdrMetadataEXT`，同时验证 OHOS WSI
  到 NativeWindow 的颜色/元数据传递；没有此扩展时，仅在官方/实测确认窗口 API
  能正确传递元数据的路径开放。建立一个元数据写入 owner，避免两套接口相互覆盖。
- 新旧 swapchain、颜色标签和 in-flight 帧必须按代际收敛。不能在仍消费旧 SDR
  图像时把整个窗口无条件标为 PQ。WSI 无法保证切换契约时先禁用热切换。

## HDR 合成、亮度与 alpha

优先复用 Chromium/Skia 的色彩管理、HDR tone mapping 和高精度中间合成。
最终目标可为 RGB10A2，内部混合按需要使用 F16/适合混合的 HDR 色彩空间；
不能把所有中间 render pass 压成 10 位 PQ，也不能改色彩标签代替数值转换。

- HDR10 最终输出使用 BT.2020 PQ；普通网页 UI 在 HDR 场景里按 SDR reference
  white 映射，不把白底网页拉到 HDR 峰值。
- SDR white、峰值与 headroom 需要真实显示/系统信息及一致单位。现有 bool HDR
  查询不提供这些证据；接口不足时使用明确标记的实验配置校准，auto 暂不开放该设备。
  不把素材 MaxCLL、任意固定 1000 nits 或窗口 0..1 brightness 参数当成面板实测峰值。
- 首版只对不透明根窗口开放 10 位 HDR；独立透明弹窗保持 8 位 SDR，由系统合成。
  页面内部阴影/半透明元素仍在高精度合成路径处理后落到不透明根目标。
- 真正带半透明根背景的窗口必须独立判定能力，不能以两位 alpha 代替八位 alpha。
  透明窗口策略不得通过全局修改 `needs_alpha` 条目破坏不透明根窗口的 HDR blending。

## HDR10 元数据与视频输入

先以受控 PQ 测试图验证输出，再接入视频。

合成输出元数据拟沿 `SkiaRenderer → OutputSurfaceFrame → SkiaOutputDevice →
Present` 传递，与 frame/color generation 绑定。包含元数据有效位、输出参考白、
必要的 SMPTE ST 2086 / CTA-861.3 字段；协议内用显式字段并检查范围、有限值和长度，
不直接序列化 SDK struct 的原始内存作为跨进程 ABI。

视频素材 metadata 不能直接当成最终混合画面的 metadata。经过色域转换、色调映射、
多视频/UI 合成后，应按输出策略生成/合并；未知值保留未知语义，必要默认策略明确
记录，不能伪造测量值。HDR→SDR、空 metadata、视频切换、GPU 重启都要清理旧状态。

视频输入分两级验收：

- 正确性：已有 buffer mode 保持 P010 的 10 位精度、range、matrix、primaries、
  transfer 和 HDR metadata，验证上传到 Vulkan/合成后的亮度与颜色。若任何环节
  降为 8 位，该用例不能计为“端到端 10 位”。
- 性能：补原生 Vulkan 的 NV12/P010 SharedImage representation，导入
  OH_NativeBuffer，验证 sampler YCbCr conversion 或多平面采样、decode acquire
  fence、GPU 读完后的 release 同步和解码器归还时机。不能只删除 kGL 判断就开放。
  GLES WebGL 与 Vulkan 合成的跨 API 资源也单列回归。

首版 HDR 输出可以先完成，HDR 视频及其零拷贝完成度分别报告。

## 切换与回退

每个输出窗口维护模式代际，流程为：

`SDR8 → 探测目标 → 准备新目标 → 更新能力/Reshape → 提交新模式首帧 → ACTIVE`

同尺寸也可能因色彩变化重建。停止旧代际申请，使用已有 fence 退休资源；迟到的
旧 lease/Present 不得提交到新颜色代际。一次转换内不能混用旧色彩的 Skia surface、
新 buffer format 与上一段视频的 metadata。正常每帧不增加 CPU WaitIdle。

首版失败策略采用保守的模式锁定：

- HDR/PQ/元数据或 10 位失败：退回 **Vulkan SDR8**，锁定本会话，记录准确原因。
  10 位 SDR 若需继续测试，在新会话明确选择，避免发生故障时自动多级试错。
- Vulkan 设备/上下文丢失：退 **GL SDR8**。先在 browser 锁定 SDR 能力、保证下代
  GPU 获得 SDR 输出状态，再允许 Viz 重建；不能留下 10 位请求驱动 8 位 GL 目标。
- 无法原地安全切换时，受控重建输出/GPU；提前锁定下一代模式，防止循环崩溃。
- 重启后可重新探测；显示器、窗口或 GPU generation 改变时旧能力失效。

现有 `--ohos-sdr-output` 保留作显式强制 SDR/故障锁定。经过实验门禁与探测的
Vulkan 会话才放开硬编码 SDR，不能删除所有设备上的保护。

## 实施顺序与交付

| 阶段 | 交付 | 进入下一阶段的条件 |
|---|---|---|
| 0 能力与状态 | 真实窗口探针、能力日志、窗口/GPU/color generation、回退状态 | 精确识别不支持项；SDR 基线和失败路径正常 |
| 1 手机 10 位 SDR | 协议与分配/import/Skia 格式扩展，10 位渐变测试 | 真实 RGBA1010102/VkFormat 一致，像素精度及色序通过 |
| 2 手机 HDR10 | PQ 输出、参考白/headroom、元数据、SDR/HDR 切换 | 真实 HDR 显示与亮度验证、透明内容及回退通过 |
| 3 Pad/PC | 10 位/HDR 格式对、swapchain 重建、元数据 | 该设备 WSI 支持并通过独立验收；手机成功不代替它 |
| 4 HDR 视频 | P010 全链路正确性，随后 Vulkan SharedImage 零拷贝 | 色彩/亮度与同步正确，解码器无阻塞，资源无持续增长 |

共用能力协商在阶段 0 建立；每阶段提供单独测试 HAR，保留 SDR8 对照。
不以最终缓冲区 10 位或 HDR API 返回成功代替 HDR 视频和系统实际显示验收。

## 验收与测试

- 自动测试：格式/位序映射、完整 VkFormat/ColorSpace 配对选择、协议版本和 FD
  校验、元数据越界/NaN/缺失、过期 color generation、失败锁定及 GPU 重启状态。
- 测试矩阵：手机与 Pad/PC 分开；SDR8、SDR10、HDR10；透明/不透明；旋转、
  resize、多标签、HDR/SDR 视频切换、前后台、显示器切换及受控 GPU 崩溃。
- 启用证据：GaneshVulkan、独立 GPU、实际分配或 swapchain 的 10 位格式、
  SkColorType、BT.2020/PQ、实际 metadata、active color generation 和首帧结果。
- 精度：生成超过 256 级的高精度渐变与固定像素值，测试工具专用读回核对有效位、
  通道次序和裁剪；生产路径不读回。8 位源素材不能证明 10 位精度提升。
- 视觉/HDR：PQ 测试图、近黑、高亮、色块与已知 HDR10 素材；核对 SDR 白底亮度、
  高光层次及色域。系统/面板 HDR 状态与校准测量是实屏证据，普通截图/录屏通常
  经 SDR 转换，只用于发现倒置、明显偏色等问题。
- 性能：120 Hz 预算 **8.33 ms**，记录显示帧间隔 P50/P95/P99、超预算比例、
  实际掉帧、GPU 时间、队列等待、内存/FD、温度与功耗。RGB10A2 仍为 4 B/像素，
  但 F16 中间层、tone mapping 和视频导入可能增加成本，不预先承诺与 SDR 等速。
- 任一 HDR 失败后必须仍可继续浏览，画面回到正确的 SDR，且不反复重启 GPU。

## 核对来源与待验证项

代码依据为当前仓库 `4ec4b24` 的 `ohos_screen`、phone shared transport/buffer、
phone Vulkan output、`ohos-vulkan-sdr-output.patch`、`ohos-video-zero-copy.patch`、
`ohos_video_frame_converter`；以及 Chromium 154 固定上游
`b510e9d7cd3a2fbd78d0ddc42234103206c5f78d` 的 VulkanSurface、DisplayColorSpaces、
SkiaRenderer、OutputSurfaceFrame 和 FrameData。

通过 `devecocli docs read` 核对：

- `API参考/Vulkan/Vulkan开发指导/Vulkan开发概述/vulkan-overview`：
  VK_OHOS_surface 对接 NativeWindow；VK_OHOS_external_memory 支持 NativeBuffer 共享。
- `开发指南/ArkGraphics_2D_方舟2D图形服务/图形缓冲区/NativeWindow开发指导_C_C/native-window-guidelines`：
  Request/Flush、buffer 配置、producer/consumer fence 语义。

本机 SDK `native_window/external_window.h` 和 `native_buffer/{native_buffer,buffer_common}.h`
提供 Set/GetColorSpace、Set/GetMetadataValue、HDR10 type、ST2086/CTA861 静态结构及
RGBA1010102 定义；相关 API 存在不代表当前设备与当前窗口路径实际可用。
窗口/Buffer 颜色接口非线程安全，需单序列调用并核对返回值。

阶段 0 必须回答：当前 Maleoon 的 10 位导入结果、Pad WSI HDR 格式对、元数据
是否真正到达系统合成器、切换时旧帧标签行为、系统参考白/headroom 来源及 Vulkan
P010 sampling 支持。设计目前不把这些未知项写成已支持能力。
