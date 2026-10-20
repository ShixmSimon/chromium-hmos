# OHOS 性能优化价值复核

日期：2026-10-10。审阅远端 `hmos-154-adapter` 的
`e021af031fbbc24c5f67d2d9cb3b57ac913c557e`（Chromium 154.0.8037.97）。
本地工作分支仍在 `74234b3`，通过读取远端 Git 对象检查新增代码，没有切换工作树。
本轮没有设备测试、构建或运行时代码修改。

## 推荐顺序

| 顺序 | 项目 | 证据与价值 | 成本 / 风险 |
| --- | --- | --- | --- |
| 1 | 浏览器状态从固定轮询改为事件驱动 | 最新代码仍每 200 ms 遍历窗口、标签、构造完整 JSON，再比较是否变化；静态页面也做这些工作。旧真机 trace 中整轮任务平均约占一个 CPU 核心的 1%，是目前量化依据最明确的一项 | 中；必须迁移轮询里的副作用，不能直接删除定时器 |
| 2 | 缩略图编码异步化、请求合并 | 截图回调同步 PNG 压缩和 Base64；影响总览、频繁切标签时的主线程响应。已有 12 项缓存和 720 px 宽度上限，后台标签已经优先取缓存 | 小到中；移线程减少主线程阻塞，去重才减少总计算量 |
| 条件优先 | shader 缓存失效与反复编译 | 用户此前报告 101 次 cached shader compilation error；现有补丁在失败时调用 `GrShaderCache::Clear()` 清空内存缓存。若最新版仍复现，应先定位错误与失效范围 | 先调查再修改；尚不能把历史日志视为最新版的持续故障 |
| 3（通话 / 录制） | 摄像头捕获缓冲复用、减少格式搬运 | 每帧 MapPlanes 后创建临时 vector、逐 plane 复制，交给 Chromium 捕获入口；硬编又 ConvertAndScale 到 NV12，拒绝 SharedImage 输入 | 缓冲与转换合并为中等工程；Surface 硬编输入为较大工程 |
| 4（持续播放） | 音频按场景选择延迟模式 | 所有输出流均请求 FAST，回调持锁完成拉取、音量处理和 S16 交错转换；普通媒体与 WebAudio / RTC 尚未区分 | 中；需测真实块长、回调频率、underrun、音画同步 |
| 实验项 | 编译优化与 V8 启动配置 | 仓库配置仍为 optimize_for_size=true、v8_enable_builtins_optimization=false、use_v8_context_snapshot=false | 先核对 runner 的有效参数，再分项 A/B；包体、内存、启动与吞吐需要权衡 |
| 长期 GPU 项 | 视频独立图层 / Overlay | Ozone 使用 StubOverlayManager，硬解 SharedImage 只声明 DISPLAY_READ / RASTER_READ / GLES2_READ；当前零拷贝消除了输出 CPU 拷贝，但仍参与 Chromium GPU 合成 | 大；需 OHOS 图层提交、遮挡裁剪、同步、HDR、截帧及回退协议 |

## 1. 状态事件化：建议下一项直接做

最新源码位置：`overlay/chrome/browser/ui/ohos/aura_shell_runtime_bridge.cc`：

- 第 290 行：`kBrowserStatePollInterval = 200 ms`。
- 第 2971 行起：`PollBrowserStateOnUiThread()`。
- 第 2987 行：每轮 `ApplyUserAgentToAllTabs()`。
- 每个窗口处理无障碍、viewport inset、工具栏和全屏状态，然后生成快照。
- 第 3104 行附近：无论快照是否变化，继续投递下一轮。

旧 trace 的 469/482 ms 线程 CPU 时间发生在约 49 秒采样窗口内，详情见
[CPU 调查](OHOS_CPU_OPTIMIZATION.md)。这是旧版整轮任务开销，包含真实工作，
不是承诺全部可消除，也不是新版实测，更不是“整机省电 1%”。

首步保持完整 JSON 协议：用标签、导航、加载、标题、音频、分组等事件标记
dirty，同一轮合并更新。外壳重订阅、恢复和窗口创建时强制发完整快照。
UA、viewport 重试、工具栏、无障碍与单标签分组维护分别迁移到对应生命周期。
加载进度允许节流；静态窗口不持续全量扫描。

验收：1/20/100 标签下比较静态页主线程 CPU、唤醒和任务 P95/P99；回归键盘
inset、全屏、读屏、标签淘汰与恢复、分组和外壳重新订阅。不能只看 JSON 通知数。

## 2. 缩略图：优先减少主线程尖峰

最新 bridge 第 1951 行 `EncodeThumbnail()` 做 PNG 与 Base64；第 2044 行
`OnThumbnailCaptured()` 直接调用它。第 1927/1930 行已有缓存数与尺寸上限。
它只对可截图的当前标签执行 CopyFromSurface；不能描述成所有后台标签都重截。

建议保留 GPU 异步回读，将编码放到有界后台序列，按 tab、内容代际、尺寸合并请求。
回到 UI 线程时检查标签/窗口和请求代际，避免已关闭标签或旧请求污染缓存；
无痕窗口关闭后的晚到任务也不得重新写入无痕图片。先保持现有 pngBase64 协议。

验收：重复开关总览、快速切标签、截图期间关闭标签和无痕窗口；统计重复捕获数、
编码次数、主线程长任务及峰值内存。本轮没有总览 trace，收益尚未量化。

## 3. GPU：先解决可重复开销，再评估新后端

`patches/ohos-cached-shader-no-crash.patch` 防止缓存 shader 错误直接结束手机
单进程浏览器，但其 Clear() 遍历清空全部内存条目。下一步应在最新版区分：

1. 同一个 shader 反复失败，还是不同 shader 各失败一次；从源码编译是否也失败。
2. 内存缓存清理后，磁盘缓存是否再次带回同一问题条目。
3. GPU/驱动/ANGLE/后端变化后，缓存兼容性标识与失效流程是否正确。
4. 重编译在冷启动、滚动、视频切换中的耗时和长任务占比。

这段补丁本身不足以证明磁盘缓存一定存在问题，也不能以“只清理单条”替代必要的
全局兼容性失效。错误需由最新日志确认，不能只把日志关掉当成优化。

Overlay 的代码依据：`overlay/ui/ozone/platform/ohos/ozone_platform_ohos.cc:120`
与 `overlay/media/gpu/ohos/ohos_video_frame_converter.cc` 的 SharedImage usage。
先验证 OHOS 提供给本应用的独立图层能力，再从符合条件的全屏视频试验。
进入系统合成也不保证硬件直出：最终由 RenderService/显示硬件决定。
不能承诺消除系统 HDR 离屏绘制，更不能通过关闭 HDR 获取表面性能收益。

局部刷新也是测量项：`ohos_surface_factory.cc` 已将 PostSubBuffer 转交 EGL
基类，不能因适配层没有更多 damage 代码就断定每帧全屏重绘。应先核对运行时
EGL 扩展、实际 damage 和交换路径。

原生 Skia Vulkan 暂不排在这些项目之前。此前 chrome://gpu 字段矛盾要通过实际
后端与 raster 状态确认；更换后端不保证比现有 GL 更快，且需覆盖视频导入和同步。

## 已完成或不应重复立项

- H.264 / HEVC Main / Main10 的硬解零拷贝与 SharedImage / EGLImage 复用已有
  Mate 70 Pro+ 验证，不能再把兼容 buffer 路径的 CopyOutput 当成默认视频热点。
- NativeBuffer 闲置自动回收已实现；当前还需要的设备验收不等于未实现。
- 动态 VSync 已实现，远端另有生命周期加固和 Vulkan 接线；真实功耗收益待同条件对照。
- 远端已新增标签页淘汰、内存压力接入与会话恢复限流，详见远端
  `docs/OHOS_TAB_DISCARDING.md`。不要再建议“首次增加后台标签回收”。
- VP9 已有 row-mt 补丁，VP9/AV1 已有平台硬解能力探测；实际落到软解时，需要
  区分解码与上传耗时，不能再把增加能力探测或首次开启 row-mt 当作新优化。
- HLG 的 14/123 真实掉帧未稳定复现。BeginFrameDropped 不能直接等同于视频掉帧，
  也没有证据要求删除正常的 HLG 色彩转换。

摄像头、音频、编译参数的进一步证据与限制见 [CPU 调查](OHOS_CPU_OPTIMIZATION.md)。
建议实施顺序为状态事件化 → 缩略图；shader 若仍反复失败则插队处理。
