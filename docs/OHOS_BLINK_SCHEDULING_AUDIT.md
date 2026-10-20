# Blink / HarmonyOS 调度源码审查

日期：2026-10-16。适配仓库基线 `7096d69f`；上游 Chromium 154 固定到
`b510e9d7cd3a2fbd78d0ddc42234103206c5f78d`。本轮只审查源码和官方文档，
没有设备操作、运行时修改或编译。下列风险不等同于已测得的掉帧根因。

## 六项结论

| 优先级 | 项目 | 源码结论 | 尚缺证据 |
| --- | --- | --- | --- |
| P0 | Renderer 主线程 | 不能从源码断言被抢占；须区分任务执行、锁/IPC 阻塞和 runnable 等待。独立 GPU 的同步等待在 GPU 输出调用链，不应记到 Renderer Main 头上 | Renderer 的长任务、调度状态、同步调用栈与输入/帧的对应关系 |
| P0 | Thread Priority | OHOS 编译 Linux platform_thread，使用 nice/cgroup 路径；活动适配没有接入 OH_QoS_SetThreadQoS | nice 设置返回值、实际 nice、线程 QoS、前后台恢复是否成功 |
| P0 | CPU 亲和性 | 已检查的活动适配及 fork 启动文件没有发现显式绑核策略；不能据此判断系统分配错误 | 各关键线程允许 CPU 集、实际运行 CPU/频率、迁核与帧超时的关联 |
| P1 | Blink Task Scheduling | 未发现针对 Blink scheduler 队列优先级的 OHOS 专项重写；平台 UI TaskRunner 包装不等于 Blink 调度器 | 队列 ready→start 延迟、任务执行时间、不可抢占长任务 |
| P1 | BeginFrame / VSync | 有固定 +2 ms 相位；手机独立 GPU Acquire/Present 同步跨进程往返，时序缓存超过 100 ms 也会同步查询 | 回调/唤醒延迟、往返 P95/P99、实际显示完成时间 |
| P2 | Worker / ThreadPool | 手机帧接收循环占用一个 ThreadPool 长任务，带 MayBlock 和 WithBaseSyncPrimitives，未显式声明 presentation ThreadType | 长任务有效优先级、阻塞记账、池并发、worker 与主线程 CPU 竞争 |

## 1. 优先级映射：已确认缺少平台 QoS 接入

`patches/chromium-154-harmonyos.patch` 的 base/BUILD.gn 将 OHOS 纳入 Linux
线程实现；platform_thread.h 和 platform_thread_internal_posix.h 将 OHOS
路由到 PlatformThreadLinux / SetThreadTypeLinux。

固定版本上游 `base/threading/platform_thread_linux.cc` 的行为是：

- Background → nice 10，Utility → 2，Default → 0。
- Presentation / AudioProcessing → -8；RealtimeAudio 尝试 SCHED_RR，失败后 nice -10。
- 尝试 `/sys/fs/cgroup/{cpuset,schedtune}/chrome/{urgent,non-urgent}`；目录不存在直接跳过。
- `SetThreadNiceFromType()` 失败仅 DVPLOG(1)，其返回值在 SetThreadTypeLinux 被忽略。

因此“Chromium 请求了 Presentation”不能作为“系统已给关键线程更高调度等级”的证明。
但目前也没有真机证据证明 setpriority 一定失败，或这些 cgroup 在该系统一定不存在。
从后台 nice 恢复到前台也须验证，不能只记录一次启动设置。

`content/renderer/render_thread_impl.cc` 中 Linux/ChromeOS 的
SandboxedProcessThreadTypeHandler 通知和 IO Presentation 分支没有包含 OHOS。
这是平台路径差异，不代表应直接开启 Linux broker：当前 fork 模式、安全模型和
鸿蒙 QoS 当前线程限制都须分别考虑。

仓库 ArkWeb 扩展中存在 RendererThreadTypeHandler 和 QoS API 声明；
`config/args.plan_kirin_pc.gn` 设置 enable_arkweb=false，不能把这部分视为当前已接入。

官方 QoS 开发指导（通过 devecocli docs read 核对）：
`开发指南/基础功能/Kernel_Enhance_Kit_内核增强能力/QoS_开发指导/qos-guidelines`。
接口只能设置当前任务；Get 返回的是最近设置的 QoS，未设置也可能返回 -1，
不是完整的系统实际调度保证。需要 libqos.so；支持 Background、Utility、Default、
UserInitiated、DeadlineRequest、UserInteractive 六级。

后续接入应覆盖 ThreadType 变化、临时 override/恢复及后台切换；在目标线程执行，
检查返回值。不能只在 browser 主线程代替 renderer 设置，也不应给整个池最高等级。
音频实时策略单独保留。此轮不预设映射已验证有效。

## 2. 手机零拷贝路径仍有同步等待

相关源码：

- `overlay/ui/ozone/platform/ohos/ohos_phone_gpu_channel.cc`：
  ExchangeShared 持 Child.lock，发送请求并同步收回复；Acquire、Present、
  AllocateImage、部分控制和 Timing 查询共用串行通道。
- `overlay/components/viz/service/display_embedder/skia_output_device_ohos_phone_vulkan.cc`：
  BeginPaint 申请缓冲，Present 发送 completion fence 并等回复。
- `overlay/ui/ozone/platform/ohos/ohos_phone_gpu_shared_buffer.cc`：
  RequestBuffer 配置超时 200 ms。
- `ohos_phone_gpu_transport.h`：通道故障保护超时 2000 ms。

上述数字是上限，**不是每帧固定等待时间**。共享路径传递缓冲与 fence，
没有因此变回逐帧 CPU 像素拷贝，但依赖主进程接收线程及时被调度。
同一通道上的请求也存在队头阻塞风险。

ReceiveFrames 用 ThreadPool 长任务运行，主进程处理共享消息时持 Browser.lock，
其中包括 RequestBuffer；该锁也用于连接和部分状态访问。
即使工作不在 UI 线程，其他拿同一锁的调用也可能受影响，须追踪实际持锁时间。
原有 500 ms surface 等待限于启动 color probe，不是每帧等待。
旧 readback presenter 的 poll/逐行 memcpy 是回退路线，不能算作共享路径必经开销。

优先补 acquire/present 分段计时和线程调度证据。若确认接收线程延迟，
再评估专用可命名 presentation 线程；若确认协议往返成为瓶颈，再设计异步化。
不能简单删回复、提前复用 lease 或绕过 fence。

## 3. VSync 相位须按 120 Hz 复核

`overlay/ui/ozone/platform/ohos/ohos_vsync_provider.cc`：

- timebase 固定加 2 ms，目的是等待 ArkUI 触摸转发；这是相位偏移，不是 CPU sleep。
- 请求范围 60..120 Hz，读取真实 period；无有效时序时默认 60 Hz。
- 空闲 500 ms 后停止持续请求；稳定状态每秒检查 period，转换期更频繁。

120 Hz 一周期约 8.33 ms，2 ms 约为其 24%。不能据此直接断言慢 24%，
也不能保证剩余 6.33 ms 全部可供 Blink 使用；RenderService 截止时间、DVSync、
GPU 和显示链都需要测量。原先偏移的收益依据在源码注释中，本轮未复核原始 trace。
建议同一输入负载做 0/1/2 ms 对照，比较输入到显示延迟和错过显示截止的帧数，
而不是只看 rAF FPS。不要直接删除这个已有目的的补偿。

## 4. Renderer / Worker 的判断边界

Blink 高优先级任务通常不能抢占正在执行的 JS 或布局任务。需区分：

1. 当前任务实际执行太久：检查 JS、样式、布局、GC、脚本注入等调用栈。
2. 队列有任务但未选择：检查任务队列政策、页面可见性、生命周期和 budget。
3. 线程已经 runnable 但没有 CPU：检查 OS 调度、QoS、竞争、温控。
4. 线程睡眠/等待锁/IPC：检查等待对象及负责唤醒的依赖线程。

不能把 Chromium task priority、平台 ThreadType 和鸿蒙 QoS 当成同一层。
Worker 数量或迁核次数本身也不能证明不合理；需要 CPU 时间和关键帧关联。
本次搜索不是完整上游线程实现的穷尽证明，未发现不等于不存在其他路径。

旧优化文档中 200 ms 全量浏览器状态轮询已经过时：当前 bridge 是事件更新加
2 秒兜底，后台停轮询；缩略图编码已投递后台任务。它们不是 Renderer Main
上已确认的旧热点。翻译 runtime 的 4 ms/64 ops 是协作调度目标而非硬抢占上限，
开启翻译或广告过滤时仍应单独采样，不预设这些功能就是问题来源。

## 最小真机验证计划

先对同版本、同网页比较单进程 / renderer 独立 / renderer+GPU 独立三组。
只比较各模式实际支持的相同后端；GL/Vulkan 的影响另设单变量对照，避免混淆。
固定实际刷新率、亮度、温度范围；每组重复滚动、动画、视频、worker 压力和
前后台恢复。一次基线、一次开启新增网页功能，区分平台开销与功能开销。

同时采集 Chromium tracing（toplevel、blink、renderer.scheduler、cc、viz、gpu、
input、latency 等可用类别）和系统调度 trace；核对设备支持的 sched_switch /
sched_wakeup、CPU 频率事件及时间基准。无法取得系统调度事件时，明确不能判断
runnable 等待和迁核原因，不用 Chromium slice 墙钟时间冒充 CPU 时间。

记录每个角色的 PID/TID（Renderer Main、compositor、Viz/GPU、帧接收、IO、
worker）、请求 ThreadType、nice/QoS 设置结果、允许 CPU 集。
补充请求 sequence 对应的 acquire/present 发送、接收、RequestBuffer、Flush、
回复阶段耗时，以及 VSync timestamp/callback/BeginFrame/presentation 时间。
统计 P50/P95/P99/最大值和超出实际帧截止的次数；不累加嵌套 slice。

决策顺序：优先级设置可观测性 → 确认 QoS 映射及恢复 → 帧接收依赖线程 →
相位对照 → 有证据后优化任务/并发。暂不固定绑大核，不用全体升优先级掩盖依赖问题。
