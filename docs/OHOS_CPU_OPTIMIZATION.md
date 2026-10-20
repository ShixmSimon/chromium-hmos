# OHOS CPU 优化调查

调查日期：2026-10-09；源码基线 `0056d8ed`。本轮只调查，不修改运行时行为。
检查当前 Aura/Ozone/AVCodecKit 路径；`enable_arkweb=false` 下不编译的 ArkWeb
扩展不作为当前内核热点。没有操作设备，也没有新的功耗或 CPU 采样。

## 优先结论：移除浏览器状态的固定全量轮询

`aura_shell_runtime_bridge.cc` 的 `kBrowserStatePollInterval` 为 200 ms。
`PollBrowserStateOnUiThread()` 每轮遍历所有浏览器窗口，调用
`ApplyUserAgentToAllTabs()`，更新 viewport/控件/无障碍接入，生成各窗口完整状态。
`BuildBrowserStateJson()` 再遍历所有标签，读取 URL、标题、音频、分组等信息，
构造字典并序列化。完整 JSON 与上一份比较之后才决定是否通知外壳。
不变化只省掉通知，没有省掉扫描和序列化；入口没有按 app/window 不可见状态停止轮询。
支持标签分组的窗口还会每轮额外投递一次 `DissolveSingleTabGroupsForWidget()`。

复核外壳提供的 `build-010f7882` 原始 trace，文件与校验和见
[HLG trace 分析](OHOS_HLG_TRACE_ANALYSIS.md)。筛选 `CrBrowserMain` 的
`ThreadControllerImpl::RunTask` 完整事件，其 `src_file` 为
`chrome/browser/ui/ohos/aura_shell_runtime_bridge.cc`、`src_func` 为
`PollBrowserStateOnUiThread`。这是该函数再次投递自身的任务来源，代表整轮工作，
不能把它全部归因于 JSON 序列化。

| 指标 | HLG 完整场景 | SDR 完整场景 |
| --- | ---: | ---: |
| 记录跨度 | 48.840 s | 48.801 s |
| 状态轮询任务数 | 238 | 239 |
| `tdur` 合计，即记录到的线程 CPU 时间 | 469.362 ms | 481.805 ms |
| 单次平均线程 CPU 时间 | 1.972 ms | 2.016 ms |
| 单次任务墙钟时间 P95 | 3.928 ms | 3.942 ms |
| 单次任务墙钟时间最大值 | 7.562 ms | 11.754 ms |

这相当于采样窗口内平均约一个 CPU 核心的 0.96%/0.99%。这些任务在所记录的
浏览器主线程任务来源中 CPU 累计最高，但不是整个应用所有线程的完整 CPU 排名。
最大墙钟时间包含被调度出去的等待，不能写成实际执行了 11.754 ms，或直接对应
视频掉帧。HLG/SDR 相近，说明这是两者共有的适配层开销。
来源为 `BuildBrowserStateJson` 的另 238/239 个任务只有 7.998/7.894 ms CPU；
它们是该函数投递的分组维护任务，不是 JSON 构造函数自己的执行计时。

建议实现顺序：

1. 用 TabStripModel/WebContents、导航、标题、加载进度、静音/可听状态、分组、
   书签、缩放、焦点、侧栏与全屏事件标记 dirty；同一轮任务合并通知。
2. 先保持外壳现有完整 JSON 协议，只在 dirty 时生成，减少兼容风险。
   活跃加载的进度更新节流；静态页面不维持 5 Hz 全量扫描。
3. 新增外壳订阅、窗口恢复和生命周期重连时主动发一份完整快照。
4. 将轮询承载的副作用拆出去：UA/网页观察者绑定走标签生命周期，单标签分组清理
   走分组变化，viewport 丢失更新使用有截止条件的重试，无障碍沿自己的 dirty 通知。
   不能直接删掉整轮任务，或只改大间隔导致键盘 inset、工具栏与状态恢复失效。

收益目标是减少无效扫描、分配和主线程周期性占用；上述全部 CPU 时间不等于可以
全部节省，因为真实状态变化仍要处理。风险中等，主要在事件覆盖和初始化/恢复语义。

## 后续候选项

| 优先级 | 项目及已确认代码 | 建议方向 | 主要收益/限制 |
| --- | --- | --- | --- |
| P1 | 缩略图：`OnThumbnailCaptured()` 同步调用 `EncodeThumbnail()`，做 PNG 压缩和 Base64；已有 12 项缓存、720 px 上限，后台标签复用缓存 | 将编码移到有界后台任务，合并重复请求，按内容/尺寸复用；保留 tab ID、请求代际及无痕清理语义 | 降低打开总览时的主线程尖峰；移线程本身不减少总 CPU，减少重复编码才减少计算。尚无该场景采样 |
| P1（录制/通话） | 摄像头 `ProcessFrame()` 等 fence、MapPlanes、每帧创建并清零 `std::vector<uint8_t>`，逐 plane 复制，再交给 `OnIncomingCapturedData()`；硬编 `FeedInputBuffer()` 每帧 `ConvertAndScale` 到 NV12，拒绝 SharedImage 输入 | 先写入 Chromium 可复用捕获缓冲，去掉临时帧和重复格式转换；再评估 NativeBuffer → 编码 Surface 的完整生命周期及同步支持 | 主要改善 getUserMedia/WebRTC/录制，不改善普通视频播放。Surface 编码属于较大工程，不能只删除转换 |
| P1/P2（条件触发） | 无障碍已有按需启用、dirty 检查；首次系统查询后 `requested` 保持为 true，PageMode 持有完整 AX 模式，隐藏只清快照，没有释放 mode | 按系统无障碍服务和实际消费者生命周期管理 mode；dirty 时避免不必要的全树复制，评估增量节点与几何更新 | 适用于读屏/自动化使用后或大页面；不能误称未使用无障碍也每 200 ms 全量构树，也不能按简单超时关闭读屏 |
| P2 | 音频输出所有流均请求 FAST；默认 48 kHz/480 帧，回调持锁完成 pull、音量处理和 S16 交错输出 | 区分媒体播放与交互音频的延迟需求，查询并匹配实际系统回调块长；再检查 FIFO、转换及锁竞争 | 降低播放时唤醒频率的候选；480 帧是引擎默认块长，不证明系统恰好每秒回调 100 次。WebAudio/RTC 必须保留低延迟 |
| P2 | OHOS 当前复用 Linux ThreadType 路径，活动适配源码未见 `OH_QoS_SetThreadQoS` 接入 | 先测线程 runnable 等待和有效调度等级，再按 Chromium ThreadType 接平台 QoS，并随状态恢复 | 主要改善负载下响应；提高优先级不减少指令数，也可能增加功耗。不要把所有线程提到最高或固定绑大核 |
| P2 | 仓库 GN 配置和保存的 runner 配置为 `optimize_for_size=true`，V8 builtins 优化关闭；JIT/Sparkplug/Maglev/TurboFan 已开启 | 核对实际编译命令，再做性能优化等级、ThinLTO/PGO 的独立 A/B；先对热模块验证，使用匹配当前版本/平台的 profile | 页面脚本、解析、排版有潜在收益；不能把 GN 声明当作当前所有目标的有效编译参数，也不能未经测试把全库切到 O3 |

摄像头带宽的量级：1080p NV12 每帧约 3.11 MB，30 fps 的单次整帧复制搬运
93.3 MB/s 的有效数据，读加写约 186.6 MB/s，尚未计临时缓冲清零和后续转换。
这是尺寸算术，不是真机带宽或功耗测量，也不是当前硬解播放路径的开销。

已进一步核对固定 Chromium 版本的
[`VideoCaptureDeviceClient::OnIncomingCapturedData`](https://github.com/chromium/chromium/blob/743f26418a267dd97c3c1c71d786038ae68cfc8f/media/capture/video/video_capture_device_client.cc#L353)：
它另从捕获池申请 I420 缓冲，再调用 `libyuv::ConvertToI420`。因此这里不仅有
OHOS 临时帧复制，还存在进入 Chromium 捕获池的转换/复制；随后若走当前硬编，
又需要 NV12 输入。首步可以合并采集打包与池内 I420 转换，同时保留旋转、
色彩空间、stride 校验和 buffer 生命周期，之后再推进 Surface 输入。

平台 QoS 的文档依据为 DevEco CLI 本地文档
`API参考/基础功能/Kernel_Enhance_Kit_内核增强能力/C_API/头文件/qos_h/capi-qos-h`：
`OH_QoS_SetThreadQoS` / `GetThreadQoS` / `ResetThreadQoS` 从 API 12 提供，
操作当前线程；接入须在对应线程执行，并覆盖取消/恢复及失败。

## 已有优化与本轮不应重复做的工作

- `0056d8ed` 的动态 VSync 请求已经交付，CPU 侧减少无效唤醒的收益仍待真机对照。
- H.264/HEVC 硬解零拷贝及 SharedImage/EGLImage 复用已验收；`CopyOutput()` 属于
  兼容 buffer 路径，不能再把它当成每个硬解视频都在执行的热点。
- VP9/AV1 已有平台硬解能力探测；实际软解时应分别采样 dav1d/libvpx 解码和上传。
  OHOS 已有 NV12/P010 NativeBuffer 上传支持，不应重复提出“首次增加 P010 上传”。
  要进一步减少软解输出复制，需核查解码器 buffer allocator、布局和帧持有约束，
  不能假设解码器可直接写任意 NativeBuffer。
- 触摸 MOVE 已按目标/指针合并尚未派发的事件；页面滚动位置读取也已在停止滚动
  300 ms 后防抖触发。继续优化之前先测排队和实际调用数。
- JIT 不再默认关闭；启动会检查可执行内存能力或遵循显式 jitless 配置。
- GPU raster 已开启。手机单进程仍包含多个线程，不能解释成所有工作串行。

## 验收与度量

优先做状态事件化与缩略图编码。每项独立对照，保留外壳默认配置。
至少覆盖 1/20/100 标签下的静态页、加载/滚动页、标签总览、前后台切换，
以及相同实际 VSync 周期的视频稳定播放。摄像头与音频另开专项场景。

测量浏览器主线程、renderer、compositor、GPU、音频/解码线程各自的线程 CPU 时间、
任务 P95/P99、唤醒次数、runnable 等待、真实掉帧及功耗；时长、温度、亮度和
系统帧率设置保持一致。trace 的 `dur` 是墙钟跨度，`tdur` 才是所记录的线程 CPU
时间；嵌套 slice 不可简单累加，来源任务统计也不等于整个应用 CPU 总量。
当前没有 `0056d8ed` 的设备数据，不把旧 trace 当作动态 VSync 后的结果。

状态事件化必须回归地址/标题、加载进度、前进后退、音频标识、标签创建/关闭/移动、
分组、书签/缩放、侧栏、全屏、软键盘/viewport inset、无障碍树、窗口恢复及外壳重订阅。
目标是静态页面无固定全量快照工作，同时外壳可见状态更新及时且完整。
