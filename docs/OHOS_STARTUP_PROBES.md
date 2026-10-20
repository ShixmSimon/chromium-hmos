> 已撤除：以下内容仅为 2026-10-19 调查期间的历史记录，不代表当前版本仍有探针。
> 最终结果和限制见 [启动速度调查](OHOS_COLD_START_INVESTIGATION.md)。
> 计时、BundleMgr 工作线程、FD 文件追踪、Dawn 实例层枚举均已从生产代码移除。
> 正式启动握手仍负责 ready / 首帧确认和失败保护，不是诊断日志探针。

# OHOS 冷启动探针

2026-10-19：用于跟进 [冷启动调查](OHOS_COLD_START_INVESTIGATION.md)。本轮仅增加观测；没有调整 4000 ms 正文请求超时、500 ms 轮询、30 秒启动判定、进程策略、GPU 后端或遮罩逻辑。没有操作设备。

## 代码位置与启用范围

- `overlay/base/ohos_startup_probe.h`：OHOS 原生阶段日志，其他平台宏为空。每个 scope 调用点最多记录前 8 次，每个 mark 调用点最多前 64 次；自动记录，无新增启动参数。
- `patches/ohos-startup-probes.patch`：GPU 初始化、GL 静态绑定、GL 无扩展初始化、EGL display / eglInitialize、图形信息采集、Dawn、Vulkan。已接入全量应用、增量构建及上游补丁检查脚本。
- `overlay/chrome/browser/ui/ohos/aura_shell_runtime_bridge.cc`：前 64 次正文请求的原生接收和 renderer 脚本回调。记录 requestId，不记录 URL 或正文。
- phone GPU broker 与 Graphite output：在原有首次成功提交分支增加时间戳标记，不新增每帧日志。
- Lumie `PageTextRequests.ets` / `BrowserRuntime.ets` / `StartupHealth.ets`：仅启动调用传入诊断参数；每个 runtime 最多诊断 64 个正文请求，超时 ID 有界保留用于识别迟到响应。普通正文读取不增加外壳诊断日志。

## 日志字段

原生日志前缀为 `OHOS StartupProbe`：

- `phase=begin/end/mark`，`stage` 为固定阶段名。
- `detail`：scope 的 0 起始调用序号；正文 mark 的 requestId；Graphite 首帧 mark 的色彩模式枚举；phone 首帧为 0。
- `pid/tid`：发出事件的进程与线程。
- `mono_us`：`CLOCK_MONOTONIC` 微秒；原生事件排序及跨进程时差使用此字段。
- `unix_us`：`CLOCK_REALTIME` 微秒，用于与外壳 `unix_ms` 对齐。若系统校时，不能把墙钟差当作可靠耗时。
- `elapsed_us/thread_cpu_us`：scope 的经过时间和当前线程 CPU 时间；不适用或时钟失败为 -1。CPU 时间不是 GPU 执行时间，也不是整个进程 CPU 时间。

按 `pid + tid + stage + detail` 配对 begin/end。嵌套阶段不能直接累加；时长包含 begin 日志的少量开销。所有探针保留 errno，不调用 eglGetError，也不额外访问系统能力。end 仅表示离开作用域，包括提前返回；不代表初始化成功，需结合原有成功／错误日志。

GPU 日志可能延后转发，应使用负载里的时间戳而非 hilog 收到消息的时间。缺失 begin/end 也可能是日志未捕获，不能单凭缺失断言崩溃。

外壳日志 tag 为 `Lumie/StartupProbe`（以现有 Logger 的前缀格式输出）：

| stage | 意义 |
| --- | --- |
| `health_start` | 启动模式探测开始 |
| `health_gate` | session ready / surface gate 改变；session 未 ready 时 surface 未读取，字段为 false |
| `health_read` | 发起一轮探测，带 attempt |
| `page_text_send` | command 排队前，带 request、unix_ms、timeout_ms |
| `page_text_reply` | 正常响应，空正文也是响应 |
| `page_text_timeout` | 4000 ms 定时器触发，仍按原语义返回空串 |
| `page_text_late_reply` | 已超时请求后来收到响应，不再次 resolve、不影响后续请求 |
| `health_result` | token 是否匹配及 health 是否已停止；不输出 token |
| `mask_release_requested` | modeProbePending 变为 false，要求 UI 撤下遮罩的状态变更 |

外壳 elapsed_ms 采用 Date.now 墙钟，保持原计时方式；轮询在后台暂停的原逻辑未改。正文只记录长度，不记录内容。

## 如何定位现有疑点

1. 从 `Gpu.InitializeAndStartSandbox`（或 `Gpu.InitializeInProcess`）下钻到 `GL.StaticBindings`、`GL.NoExtensions`、`GL.InitializeDisplay`、`GL.eglInitialize`、`Gpu.CollectGraphicsInfo`、`Gpu.InitializeDawn`、`Gpu.InitializeVulkan`。若某段 elapsed 约 1.2 秒而 thread CPU 很低，可把等待定位到该客户端阶段；不能据此单独判定 GameAwareLayer 或具体系统服务。
2. 按时间与 requestId 对齐 `page_text_send` → `PageText.NativeReceive` → `PageText.ScriptReply` → `page_text_reply/timeout/late_reply`。requestId 在不同 runtime 可重复，必须结合进程、启动时间及请求顺序。原生接收标记仅覆盖有 active WebContents 的分支；没有标记也可能是无 active tab 或日志截断。
3. 比较 `mask_release_requested` 与 `Frame.PhoneNativeBufferAccepted` / `Frame.PhoneReadbackAccepted` / `Frame.GraphiteNativeAccepted`。phone 标记为每条连接首次成功提交；Graphite 标记可在输出色彩模式重建后再次出现。它们表示 NativeWindow/broker 接受提交，**不是屏幕真实显示时刻**，也不是新加的 readiness 信号。
4. `GameAwareLayer` 的系统 ability ID、错误码及具体 sleep 调用栈仍需后续系统级追踪；本轮未 hook 系统库，也没有禁用该 layer。

## 本地验证与限制

- 固定 Chromium revision `b510e9d7cd3a2fbd78d0ddc42234103206c5f78d` 的三个受影响上游文件，叠加现有主补丁及 video 补丁后，新增探针补丁正向／反向检查通过。
- 增量构建中在探针已存在时撤下／重叠 video 补丁，探针仍可完整反向应用。
- 三份构建脚本 bash 语法检查、两仓库 diff 空白检查通过。
- Lumie 三个修改的 ArkTS 文件检查无错误（依赖中有 15 项既有警告）。启动模式原有 30 项测试和新增 3 项正文请求回归测试通过，覆盖空响应、超时后迟到响应、请求隔离、日志内容及诊断上限。
- `devecocli build --modules entry` 本地编译与打包成功，未安装、未启动应用。
- 完整原生 Chromium 编译及设备日志验收尚未进行。外壳本地打包使用当前已有引擎产物，不表示安装包已含新的原生探针；须等原生构建后替换引擎再验收。
