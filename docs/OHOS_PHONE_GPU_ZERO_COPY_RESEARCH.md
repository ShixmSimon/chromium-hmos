# 手机 fork GPU 零拷贝调研

日期：2026-10-13。代码基线：`d35cda2cfa1ccfe68a88f8bdfa97a9806c5a8dc7`。
初版为源码、SDK 和官方本地文档调研。其后的真机探针更新见下文；内核运行代码尚未修改。

## 结论

目标是让 GPU 子进程直接绘制宿主 XComponent 队列中的缓冲区，宿主只负责申请、
传递缓冲区身份和同步 fence、提交显示，不读写整帧像素。这可以消除应用呈现链路的
GPU→CPU 读回及 CPU 像素拷贝；不代表系统合成器一定直接扫描输出，也不代表网页、
视频解码和所有渲染阶段都没有内部拷贝。

已有 EGLImage / NativeBuffer 适配可复用。当前首要障碍是 fork 子进程如何获得
可供图形驱动导入的缓冲区，而不是 socket 能否发送 FD。初始调研时尚未打通；后续独立探针已成功，当前已接入 Chromium 待外壳真机验收。

## 当前链路与目标链路

当前：

```text
GPU 子进程：pbuffer 绘制 → glReadPixels → memfd
                                      ↓ FD（SCM_RIGHTS）
宿主：pread → vector → 按行 memcpy / 上下翻转 → NativeWindow buffer → Flush
```

候选目标（宿主持有窗口队列）：

```text
宿主 RequestBuffer → buffer 身份/布局 + acquire fence → GPU 子进程
                                                       ↓ 导入 EGLImage
                                                       ↓ 等待 fence
                                                       ↓ 绘制到这块 buffer
宿主 FlushBuffer ← lease 标识 + completion fence ←───────┘
```

先使用宿主实际 `RequestBuffer` 得到的缓冲区，避免首轮同时引入外部分配缓冲区的
Attach/Detach 和队列归属问题。每次提交只传控制信息和同步对象。

## 已核实的接口与限制

| 能力 | 证据 | 对本项目的含义 |
| --- | --- | --- |
| NativeBuffer 跨进程序列化 | SDK / 官方文档：`OH_NativeBuffer_WriteToParcel`、`ReadFromParcel`，API 23 | 有官方对象传递入口，但仍需要可用的 IPC 通道；API 存在不能证明 fork 后 IPC 可用 |
| 直接从 FD 导入 NativeBuffer | 检查本机 API 26 `native_buffer.h`，未找到公开入口 | 不能把收到的 FD 转型成 `OH_NativeBuffer*` |
| Unix socket NativeBuffer 发送/接收 | 本机 SDK 未找到 `OH_NativeBuffer_SendHandleToUnixSocket` / `RecvHandleFromUnixSocket` | 不应照搬 Android AHardwareBuffer socket API 名称 |
| NativeBuffer → window-buffer 包装 | `OH_NativeWindow_CreateNativeWindowBufferFromNativeBuffer` | 可给 EGL 使用，但前提仍是先拿到合法 NativeBuffer |
| Native buffer → EGLImage | `EGL_NATIVE_BUFFER_OHOS`；已有 ANGLE 适配 | 已有基础，不必重新实现全部图形导入 |
| RGBA EGLImage 可渲染 | ANGLE 补丁 `NativeBufferImageSiblingOHOS::isRenderable` 对非 YUV 返回 true | 需实测 FBO complete、实际绘制与呈现，不能仅凭补丁返回值判成功 |
| 跨进程窗口 | 当前官方 GPU 路径用 `OH_NativeWindow_WriteToParcel` / `ReadFromParcel` | 若能建立有效 IPC，可优先尝试复用完整窗口通道 |
| fork GPU 按 SurfaceId 查窗口 | 上轮 Mate 70 Pro+ 探针返回 40001000 | 本设备本路径失败；不应推广为所有设备/系统永久不支持 |
| dma-buf 导入 | 现有 `artifacts/gpu-baseline/gpuinfo.json` 的 ANGLE EGL 扩展没有 `EGL_EXT_image_dma_buf_import` | 只是历史快照，不是当前 d35cda2c fork 进程的能力测量；需要新探针 |
| ANGLE 是否透传 dma-buf 能力 | `patches/deps/third_party_angle.patch` 的 DisplayEGL 依据底层 EGL 扩展设置 `imageDmaBufImportEXT` | 必须同时核对系统 EGL 和 Chromium 使用的 ANGLE EGL |
| Fence | NativeWindow Request 返回 fence，Flush 接收 fence；历史 ANGLE 快照有 `EGL_ANDROID_native_fence_sync` | 能力提示，不代替实际 fence 导入、导出和所有权测试 |

SDK 根目录：
`/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot/usr/include/`。

`OHIPCParcel` 不是可直接发送的普通字节数组。不能把其中 FD 数值、Binder 对象、
进程内指针原样复制到 socket 对端；`ReadBuffer`/`WriteBuffer` 不能替代对象传输。
`BufferHandle` 还有 reserveFds/reserveInts 等字段，也不能凭主 FD 和宽高猜测驱动布局。

## 候选路径与决策顺序

### A. 系统 IPC 传完整窗口或 NativeBuffer

这是与已有官方 GPU 路径最接近的架构。先验证 fork 子进程是否能通过公开接口建立
独立、有效的 IPC 连接并获得对端 RemoteProxy，再验证对象往返。

当前官方路径由原生子进程启动回调取得 proxy；手机 fork 路径没有这个引导过程。
fork 服务还会关闭继承 FD，因此不能直接复用父进程遗留的 Binder proxy/FD。
本轮没有找到并验证可替代的公开引导流程，不能写成“加上 WriteToParcel 就完成”。

若完整 NativeWindow 能合法传入并在子进程创建 EGL window surface，则优先评估复用
已有窗口呈现路径；若只能共享 NativeBuffer，则使用宿主队列 + 子进程绘制方案。

### B. dma-buf FD → EGLImage

适合现有 SOCK_SEQPACKET / SCM_RIGHTS 架构，值得先做低成本能力探针。
要求当前系统 EGL 和目标 ANGLE 支持 `EGL_EXT_image_dma_buf_import`，并且可获得
真实的 fourcc、plane offset、pitch、必要的 modifier 等布局信息。

不能把 OHOS 的像素格式枚举直接当 DRM fourcc，也不能将未报告的 modifier 默认当作
线性布局。`eglCreateImage` 成功后还要证明此格式可作为 FBO 渲染目标。

如果底层驱动不支持，增加 ANGLE 扩展字符串或 GN 开关不会产生硬件能力。
若只有系统 EGL 支持而 ANGLE 不支持，需要明确实现 ANGLE 导入或共享方案，不能把
系统 EGLImage/GL 对象直接传给另一套不共享对象的 ANGLE context。

### C. 私有结构/序列化布局重建

自行拼装 NativeBuffer、依赖内部 SurfaceBuffer ABI、解析未公开的 Parcel 字段，
都依赖系统版本及图形驱动实现。本轮不选作首选适配路径。
预先创建缓冲区再让 fork 服务继承也不适合当前早期 fork 架构：涉及图形初始化后的
进程状态继承，并不能自然解决动态窗口、尺寸变化和 GPU 重建。

### 中间方案：无 CPU 拷贝，但保留一次 GPU blit

如果成功导入目标缓冲区，可先保留 pbuffer 绘制，把最终图像用 GPU blit/绘制复制到
共享的窗口缓冲区，移除 `glReadPixels`、`pread` 和逐行 `memcpy`。
它仍有一次 GPU 图像复制，应称“无 CPU 读回/拷贝”，不能称严格的直接渲染零拷贝。
它也需要先解决缓冲区共享，不能绕过上述障碍。

## 最小真机探针：逐级判断，不先重编整个 Chromium

保持与现有生产路径相同的早期 fork 服务，窗口在服务创建之后生成，避免继承缓存
导致假成功。目标仍为 Mate 70 Pro+ / API 26。每步都输出 PID、API 返回值与图形错误。

1. **能力清单**：分别记录宿主和 fork 子进程的系统 EGL/GL 扩展、图形驱动版本；
   再记录当前内核 ANGLE 的扩展。关注 dma-buf import/modifiers、EGLImage、native fence。
   扩展字符串、函数指针非空都只算前置条件。
2. **同进程对照**：宿主 RequestBuffer → EGLImage → FBO，画四角异色图，提交到原窗口。
   验证配置、RGBA/BGRA、stride、方向。此步骤通过不算跨进程通过。
3. **跨进程导入**：依据 A/B 的前置条件选路，传递同一缓冲区到真正的 fork 子进程。
   在子进程导入、检查 FBO、绘制，宿主 Flush 原 buffer。用屏幕结果确认，不能只看
   eglCreateImage 成功。探针可以单独读回校验像素，但不得将该校验混入零拷贝性能路径。
4. **同步与生命周期**：先用 glFinish 建立同步对照，再改为 acquire/completion fence。
   glFlush 只提交命令，不代表绘制完成。检查 fence timeout、关闭规则、重复/陈旧回复。
5. **持续呈现**：100 帧基本验证后，跑至少 1000 帧、奇数尺寸、尺寸切换、前后台、
   窗口销毁重建、GPU 中途退出重启；检查 FD / DMA buffer / RSS 是否持续增长。
6. **接入 Chromium**：前五步通过才修改输出路径，在 ANGLE + Skia GL 下验证完整网页。
   能正常显示、没有应用侧像素复制、同步正确、资源稳定是不同的验收项，分别记录。

失败记录要区分：不支持扩展、buffer import 失败、FBO 不完整、fence 超时、window flush
失败、图像方向/颜色错误。导入失败就停止该分支，不以截图偶然正确判定通过。

## 接入当前代码时需要改什么

- `ohos_phone_gpu_transport.*`：新增有版本的 Acquire/Buffer/Present/Release 协议，携带
  GPU generation、widget generation、buffer lease、frame sequence 和 FD 数量。
  原来的“每帧一个 memfd”协议不能原样用于缓冲池。
- `ohos_phone_gpu_channel.cc`：由宿主管理真实窗口及借出的 buffer；按 generation 丢弃
  陈旧消息。buffer 只有经过正确同步、队列回收后才能再次交给 GPU 写入。
- `ohos_phone_gpu_presenter.*`：将 CPU Map/拷贝改为 Request/Flush 队列管理。参考 SDK
  区分 Request fence 的调用方关闭责任与 Flush fence 的系统接管责任。
- `ohos_surface_factory.cc` / Chromium 输出设备：目前绘制目标仍为 pbuffer 默认 FBO。
  仅在 SwapBuffers 中发送 buffer FD 不会让 Skia 自动改画到 EGLImage；需要接入
  输出 framebuffer/输出设备的正确契约，或先实现一次 GPU blit 的中间方案。
- `patches/deps/third_party_angle.patch`：可复用 OHOS EGLImage 导入与系统 wrapper 符号
  解析。先测试现有能力，避免重复引入一套 GL dispatch。

资源清理尤其注意：GPU 进程退出不应直接等同于所有 GPU 命令完成；不能在没有可靠
完成信号时把仍可能被写入的 buffer 立即回收复用。保持有界队列，清理不确定的资源时
按照队列/驱动契约处理，避免用猜测的完成时间。所有窗口 API 按同一序列调用。

呈现回执还要区分“已入队”和“实际上屏”；当前路径的 Flush 成功不能提供真实显示
时刻，不能作为零拷贝改造后的精确 presentation feedback。

作用范围继续限定为 phone/foldable + experimentalPhoneFork + separate GPU，
单进程默认和 tablet/PC 原有 NCP 路径不因研究而改变。Vulkan 另行验证；它也需要
可导入的缓冲区和正确的跨队列同步，切后端本身不会解决共享问题。

## 官方资料与代码证据

本轮通过 deveco-cli 读取的官方本地文档：

```sh
devecocli docs read 'API参考/ArkGraphics_2D_方舟2D图形服务/C_API/头文件/native_buffer_h/capi-native-buffer-h'
devecocli docs read '开发指南/ArkGraphics_2D_方舟2D图形服务/图形缓冲区/NativeBuffer开发指导_C_C/native-buffer-guidelines'
```

对应 SDK 头文件：`native_buffer/native_buffer.h`、`native_window/external_window.h`、
`native_window/buffer_handle.h`、`IPCKit/ipc_cparcel.h`、`IPCKit/ipc_cremote_object.h`、
`EGL/eglext.h`。

仓库主要证据：`overlay/ui/ozone/platform/ohos/ohos_native_pixmap.{h,cc}`、
`ohos_gpu_child_channel.cc`、`ohos_phone_gpu_{channel,transport,presenter}.cc`、
`ohos_surface_factory.cc`、`overlay/base/process/fork_server_ohos.cc`、
`patches/deps/third_party_angle.patch`。历史扩展快照为
`artifacts/gpu-baseline/gpuinfo.json`，不能替代当前 fork 进程探针结果。


## 真机探针更新（2026-10-13 17:40）

已在 Mate 70 Pro+ / API 26 打通共享窗口缓冲区直接绘制：主进程先通过公开 API
分配并释放一个小 NativeBuffer，预初始化图形分配器，然后启动生产 fork 服务；
窗口和实际显示缓冲区均在 fork 后创建。通过内部 Parcel 布局分离元数据和 FD、
SCM_RIGHTS 传输、接收端重建 Parcel，再由 ReadFromParcel 导入。

未预初始化的对照卡在图形服务查询，Binder 返回 -13；预初始化后 NativeBuffer、
EGLImage、RGBA FBO 全部成功。此结果不表示 fork 子进程具备通用 Binder 访问能力。
内部 Parcel 布局与继承 mapper 的长期兼容性仍是接入前必须处理的限制。

两轮 1080p、两轮 1316×2832，每轮 1000 帧成功 Flush，中途尺寸宽高各增加 1，
截图确认四色图和白色竖条正确。采用 acquire/completion native fence；没有逐帧
ReadPixels、pread/memcpy、GPU blit 或 glFinish，最后清理资源前才 glFinish。
复测宿主 FD 55→55、子进程 10→10，四个子进程均正常退出。

用户明确以 **120 Hz / 8.33 ms** 为整帧预算。全分辨率复测控制/提交阶段 P95 3.605 ms、
P99 5.474 ms；包含取 buffer 等待的整轮 P95 12.751 ms、P99 18.405 ms。
仍有长帧，未取得逐帧实际上屏或完整 GPU 完成时长，不能宣称稳定 120 fps。
缓冲区按全分辨率绘制，但探针窗口显示区域仅 80 vp 高，未覆盖全屏合成负载。
本轮为系统 GLES，ANGLE、Chromium/Skia 输出 FBO、崩溃恢复和长期稳定性仍待接入验证。

完整报告与证据：
本地交付目录 `Lumie-phone-gpu-zero-copy-20261013/REPORT.md`（不入库）。


## Chromium 接入与外壳验收（2026-10-13）

只在 `deviceClass=phone/foldable`、`experimentalPhoneFork=true`、
`gpuProcess=separate` 的现有实验路径启用。包默认单进程的策略不变。
外壳继续使用 GLES / Skia GL，切换配置后完整重启应用。

实现：在早期 fork 前初始化 allocator；宿主从窗口队列请求缓冲区，用受校验的
Parcel 元数据和 SCM_RIGHTS 传真实 FD；子进程导入 EGLImage，并直接作为稳定输出
FBO 的纹理附件供 Skia 绘制。pbuffer 缩为 1×1，仅用于维持 EGL context。
成功路径没有整帧 readback、CPU 像素拷贝或 GPU blit。

每次绘制前重新取得 lease，使用 GPU acquire wait / completion native fence。
导入缓存最多 8 项，resize 清理旧缓存；仅清理阶段 `glFinish`。
未知 Parcel 布局、缺少扩展或导入失败会回退到读回路径；原生导入若阻塞超过 2 秒，
终止该 GPU 子进程交由现有恢复策略处理，避免永远卡死。断连或未确认完成的 lease
从窗口队列 detach，不作为安全完成帧 abort 回队列；detach 失败保留引用并停用共享。

浏览器持有 NativeVSync，将实测 timebase / interval 返回 GPU；idle 释放刷新率请求，
移除手机提交反馈中的固定 16 ms。反馈仍是提交确认，不是实际显示完成时间。
共享 FBO 使用 top-left 原点；读回回退携带行方向标记，不能再固定上下翻转。

验收需要同时检查以下日志，不能只以“独立 GPU PID 存在”判断零拷贝已启用：

- `phone GPU shared-buffer allocator prepared=1`
- `OHOS phone GPU: direct shared-buffer FBO active (no readback/blit)`
- `OHOS phone GPU: first frame presented using shared NativeBuffer`

出现 `readback fallback` 说明发生回退；如果只在窗口尚未注册或尺寸变更期间出现，
需确认随后恢复共享路径。持续回退不能记为零拷贝通过。

对照 A：单进程基线；B：上一个读回内核 `d35cda2c` 的独立 GPU；C：本次共享缓冲区
独立 GPU。使用同一页面、亮度、温度和电源模式，每轮记录系统实际刷新率。
检查长页面滚动、动画、透明弹窗、WebGL、Canvas、视频、旋转/尺寸变化、前后台、
标签切换、多标签冷启动，以及至少 5 次受控 GPU 崩溃恢复。
四色图、文字和截图用于检查上下颠倒、颜色、残影及黑帧。多轮后检查宿主和 GPU FD
与内存是否持续上涨。崩溃恢复后须再次确认共享路径日志。

120 Hz 下以 **8.33 ms** 为一帧预算，报告 frame interval 的 P50/P95/P99、超过预算
的比例、实际显示帧率和掉帧；同时分开看 RequestBuffer 队列等待、GPU 执行和提交。
不能把平均提交次数、Flush 成功率或提交反馈时间当成实际显示 120 fps 的证据。

本次 Linux 协议测试覆盖 schema/整数边界、截断消息、多余 FD、FD 泄漏、fence
所有权、超时及 EOF；另做 OHOS C++ 编译检查。系统 GLES 探针成功不能替代
Chromium + ANGLE 的全屏真实页面验收。私有 Parcel 布局仍为兼容风险，上架许可另行确认。
