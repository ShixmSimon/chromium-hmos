# 手机冷启动复查：会话恢复与呈现确认

日期：2026-10-20。接续 `OHOS_COLD_START_INVESTIGATION.md`，本次未重新验证系统 Binder/EGL 根因，也未修改原生实现。

## 版本与方法

- Mate 70 Pro+（设备序列号不随报告发布）。
- Lumie `main` / `3e5e51d`，内核发布包 `build-ad1e9e45`；工作区内核 HEAD `2254e652`。
- 安装前重新核对 HAP SHA256：`f5021cd8a09cd5c16999de37da3d77a0fbf1883889ff3048b0c3b0ab19b40bd6`。其中两份原生库与 CI HAR 对应，详见 `artifacts/sync-20261020/hap-verification.json`。
- 覆盖安装成功、保留应用数据。设备安装目录不允许 shell 读取，未能直接对安装后的 so 再做哈希；版本证据来自核验过的安装包和成功安装记录。
- 3 次 force-stop 后由 `devecocli device app start` 启动；每轮保留约 42 秒，确认启动后的 30 秒稳定期完成。
- 不清缓存、不重启手机，故为进程冷启动，不是存储冷缓存启动。主机 CLI 发起时间包含 CLI/HDC 开销，不等同于桌面点击。
- 每轮启动 10 秒后才查 GPUInfo、页面数量和截图，避免这些查询主动初始化 GPU 干扰启动阶段。
- 恢复的是现有用户会话：5 个标签，活动页是 Google 搜索页。首轮外壳网址存档为 4 个，Chromium 原生存档恢复为 5 个；未把两者误认为同一份存档。
- 第 2 轮启动结束后另读了页面性能信息，后台页调试连接可能触发加载，不能据此推断正常启动会主动加载全部后台标签；该采样不作为启动期后台加载证据。

## 无临时日志的基线

| 指标 | 第 1 轮 | 第 2 轮 | 第 3 轮 |
|---|---:|---:|---:|
| StartupHealth 开始至确认，前台时间 ms | 3497 | 2682 | 3803 |
| 主机 CLI 发起至确认 ms | 4435 | 3705 | 4800 |
| 标准 GPUInfo 初始化 ms | 1667.883 | 1749.342 | 1549.762 |
| 内核恢复完成日志 → 外壳确认恢复 ms | 225 | 113 | 191 |
| 恢复后页面数 | 5 | 5 | 5 |
| 残留探针页面数 | 0 | 0 | 0 |

三轮均为 `GraphiteDawnVulkan`，无启动探针超时，均有 `restored window presented` 和 `startup stable after session restoration`。第 1、3 轮截图已人工检查，活动网页正常显示。

GPUInfo 是总 GPU 初始化时间，不能直接标成 EGL 或 Binder 等待。此前报告对系统层约 1.5 秒 EGL 的定位仍适用，本次没有新增更深的系统层证明。

第三轮活动页 Navigation/Paint Timing：

- navigation timeOrigin：`1792461250388.1` ms。
- responseStart：66 ms；responseEnd：90.3 ms；DOMContentLoaded：455.8 ms。
- first-paint / FCP：1124 ms，即约在主机发起后 3021 ms。
- 外壳确认约在主机发起后 4800 ms，比 FCP 晚约 **1779 ms**。

FCP 不是窗口实际呈现，也不是物理屏幕出光。因此这是需要解释的间隔，不是已证明可删掉的等待。当前证据也不支持把全部额外耗时归因于网络。

## 临时外壳日志确认的时序

仅临时增加 6 行日志：StartupHealth start/send/ack，以及 BrowserRuntime 收到 `lastSessionRestored`。ArkTS 检查和完整构建通过，运行后撤回，补丁留在 artifacts。

一轮无 trace 的诊断启动（GPU 初始化 1607.99 ms，StartupHealth 2286 ms）：

| 设备时间 | 事件 |
|---|---|
| 09:57:45.046 | 收到 renderer ready |
| 09:57:45.149795 | 内核已恢复 5 个标签 |
| 09:57:45.150 | 外壳收到 `lastSessionRestored`，但 session 快照仍只有 1 个标签 |
| 09:57:45.348 | 300 ms 恢复轮询发现新标签，发出关闭临时起始标签命令 |
| 09:57:45.532 | 下一次 250 ms 启动轮询才发出首个 frame 请求 |
| 09:57:46.310 | 收到 presented，id=6、latest=6 |
| 09:57:46.312 | 确认启动并解除遮罩 |

结论：

1. 恢复事件传递本身约 1 ms；这轮额外等待主要发生在外壳轮询观察与发起下一阶段。
2. **不能只把 300 ms 定时器改成收到事件就查 `session.tabs`**：事件可以先于快照到达。
3. 内核已恢复后到首个 frame 请求相隔约 382 ms，但 GPU/页面仍可能同时工作，不能把 382 ms 当成必然的端到端收益。
4. 这轮所有有效回包均与最新 requestId 匹配，没有观察到旧 requestId 被丢弃导致的延迟。

## 呈现 trace

额外一轮以浏览器级 CDP Tracing 记录 `toplevel,cc,viz,gpu,blink,loading,renderer.scheduler,devtools.timeline`；原始 trace 约 129 MB、568630 个事件。Tracing 启动应答本身明显延迟，且 trace 会影响调度，**不并入上述基线统计**。未使用这轮证明正常启动的后台标签加载策略。

- StartupHealth 耗时 4054 ms。
- frame 首请求先得到 waiting；后续在途请求更新编号，最终 presented id=13 与 latest=13 匹配。
- trace 有活动 Google 页 FCP，约在 CLI 发起后 3243 ms；FramePresented 事件约在 3316 ms、4973 ms，外壳确认约在 4952 ms。
- trace 与主机墙钟以 `Tracing.recordClockSyncMarker` 近似对齐，存在 RPC/时钟误差，不能用末尾几十毫秒的相对次序推断逻辑违约。
- 该阶段能看到页面脚本执行（单段约 207 ms、126 ms）、GPU 栅格任务等；没有发现浏览器主线程在 3–5 秒区间内单次超过 50 ms 的完整任务。不能据此排除较短任务堆积或未被 trace 覆盖的等待。
- 现有类别没有分别标明 StartupHandoff 的 `OnSubmitted` 与 `OnPresented`，因此尚不能把首次绘制后约 1.7 秒的间隔精确分摊给文档提交确认、窗口重绘或呈现反馈。

## 源码结论与下一步优先级

### 1. 先补齐呈现门控两段原生时间点

`overlay/chrome/browser/ui/ohos/aura_shell_runtime_bridge.cc` 的 `StartupHandoff`：frame 请求依次经过 `InsertVisualStateCallback`、`OnSubmitted`、`RequestSuccessfulPresentationTimeForNextFrame`、`OnPresented`。建议使用可开关 trace 事件，记录 phase/requestId 与文档身份，不记录完整网址：

- frame 请求接受 / waiting 原因；
- `OnSubmitted` 成功与否；
- 窗口呈现回调注册；
- `OnPresented` 及当前文档是否仍匹配。

这是解释剩余波动最直接的一步。不能为缩短数字而直接在 FCP 或 renderer ready 时解除遮罩。

### 2. 会话恢复改成有状态的事件驱动

Lumie `LastSessionRestorer.ets` 当前每 300 ms 看标签数量，`StartupHealth.ets` 每 250 ms 触发下一阶段。内核已有 `lastSessionRestored {count}`，外壳尚未消费它。

建议同时接收恢复结果与后续状态快照，以开始时的标签 ID 和期望新增数核实恢复结果；无论事件先到还是快照先到均能完成。完成关闭/选择命令入队后立即唤醒首帧请求，保留超时兜底，处理 count=0、窗口销毁和外部打开链接。不能依赖“任意标签数量增长”证明这次恢复已完成。

注意：BrowserSession 注释仍写“200 ms 快照”，当前内核实际上是事件驱动、100 ms 合并节流、2 秒兜底轮询，不能依据旧注释再叠加 200 ms 固定等待。

### 3. 消除会被立即关闭的起始页导航

`EntryAbility.loadSavedTabs()` 以第一个存档网址作为起始页；`StartupHealth.beginRestore()` 先导航到此网址，再调用完整会话恢复；`LastSessionRestorer` 成功后关闭这个临时起始标签。因此完整恢复路径存在可避免的导航请求。其网络/renderer 实际代价尚未量化。

仅在“普通冷启动、确实走 Chromium 完整恢复、临时起始标签随后会关闭”时跳过导航。外部 URL、桌面卡片、恢复失败按网址回退仍须正确导航；frame 阶段的 resume URL 兜底也要保留。不要直接把起始 URL 改成 active URL 而不调整 SavedTabsRestorer 的索引契约。

## 收尾与证据

- 临时外壳源码日志已完全撤回，Lumie 工作区恢复干净，仍为 main / `3e5e51d`。
- 恢复源码后完整构建成功；手机覆盖装回测试前核验过的 `production.hap`，并重新启动。
- 装回后的复核：10:01:01.926 呈现确认（foreground_ms=2651），10:01:31.989 稳定期完成；该进程日志不含临时 StartupTiming。记录在 `final-production.log`。
- 本次没有改图形后端、零拷贝/HDR/P3、系统 Binder 或 GPU 初始化逻辑。
- 原始记录：`artifacts/startup-current-20261020/`，包括 `summary.json`、`run-*.json/log/png`、活动页 timing、`diagnostic/probes.patch`、`diagnostic/trace.json`、诊断日志和构建结果。

## 后续执行：原生分段 trace 与事件驱动恢复

### 原生诊断版本

- Chromium `897896675f189a5a809af471230798929b3e2c98` 已推送，Adapter CI 与 native incremental CI 成功，发布 `build-89789667`。
- `StartupHandoff` 增加 `startup/OhosStartupHandoff` trace，包含请求、waiting 原因、visual state、呈现注册与回调；不记录 URL 或 token。未启用 trace 时不输出这些事件。没有改变成功呈现后才移除遮罩的契约。
- `document_id` 是捕获候选文档的序号，不是跨请求稳定的真实文档标识；不能用其递增直接证明标签切换。
- HAR SHA256：`bf237c4009239a34b5ee7c9b3c689007ae423439a250a165afd12e9dc2ec29a2`。
- 原始证据：`artifacts/startup-handoff-89789667/`；旧 HAR 与旧 engine 缓存保留在 `previous-engine/`。

旧外壳 `3e5e51d` 的三轮 trace：StartupHealth 为 2615 / 2618 / 2556 ms，均为 GraphiteDawnVulkan、GPU crash count=0。最终成功工作的耗时如下（第一轮是大类别 trace，不与小类别严格等价）：

| 轮次 | 接受请求→visual state | 呈现注册调用 | 注册→呈现回调 |
|---|---:|---:|---:|
| run1 | 452.319 ms | 0.016 ms | 386.582 ms |
| run2 | 448.166 ms | 0.017 ms | 406.337 ms |
| run3 | 495.913 ms | 0.021 ms | 298.524 ms |

这表明等待分别存在于 visual state 和成功呈现反馈，注册函数本身不是数百毫秒耗时来源。部分早期接受的工作未完成就被替换；可能涉及导航失效或标签变化，现有事件不足以确定原因。CDP 在主机发起后约 1.5–1.9 秒才连接，早期阶段可能缺失，且 trace 会影响调度。

### 外壳实现与验证

- Lumie `fc4ca73` 已提交并推送 main：用每窗口恢复事件和 `session.apply` 后的快照共同推进恢复，核对起始标签 ID、期望新增标签数；事件或快照先到都可处理。
- 关闭临时起始标签／切回外部链接之后，等待后续快照确认，再清除 `modeRestorePending`。保留 count=0、无事件旧引擎和选择超时兜底；页面销毁取消监听和定时器，runtime dispose 也清理监听。
- 未改 StartupHealth 的 250 ms 轮询，也未删除临时起始页导航。这两项应单独验证，不和本次状态机一起扩大改动。
- Node 24.11.1 下全量测试 **165/165** 通过，完整 `devecocli build --modules entry` 成功，装机 Smoke PASS。独立 `check arkts` 报未改动的 LibraryPages 对生成模块 BuildProfile 的解析错误；正式 ArkTS 编译成功。
- 最终 HAP SHA256：`7bf11330e6b73d6fea90e6eaeba4f7eb914220a535dfdc4cbfb99e55871c2392`；包内 libweb_engine.so / libnweb_render.so 哈希与上述 HAR 一致。

采样注意：`event-driven-run1` 与 `event-driven-run3` 是安装后的冒烟启动紧接着被强停，早于 GPU 的约 10 秒稳定确认，留下了保护标记。下一轮日志明确显示 `GPU child failed last run; in process this once`，CDP 为 GaneshGL。虽然外壳仍打印期望配置 `skiaVulkan=true`，**这不证明实际运行后端**。这两轮性能剔除，不删除或绕过保护标记。

有效 Graphite 轮次目前：`event-driven-run2` 2129 ms、`event-driven-run4` 2706 ms；均恢复 5 标签、GPU crash count=0，30 秒稳定完成，原生完成日志到外壳恢复确认约 5 / 8 ms。波动尚不足以证明稳定的端到端收益；较慢轮次仍有后续 frame 工作替换。下一步应继续定位失效原因，而不是提前解除呈现门控。

第五次补采在 `Tracing.end` 后等待完成事件超时，没有生成 trace/run JSON，不纳入性能统计。最终装机为含页面销毁取消逻辑的 `fc4ca73` 源码包（第三、四轮已使用），不回退到旧外壳。未改系统 Binder、图形配置、零拷贝、HDR 或 P3。

## 首帧失效原因定位与定向重试

### 诊断版本 `0ad22e45`

新增 `invalidate_pending` / `invalidate_idle` 原因和 native `reply` trace，覆盖同文档导航、主文档导航/提交、活动 WebContents 变化、销毁、取消和请求替换。Adapter CI `39374364118`、native CI `39374364122` 均成功。

HAR SHA256：`3c38f23e896e4d6fa797147f19f5508efbf176bd2fd6f3f267c5117d0dd37ce0`。HAP SHA256：`27ad744e62f6481948d89c3f75a34306e07230056a7644479afc2ac23ce62430`。包内两份 native 库均与 HAR 一致。Lumie 外壳保持 `fc4ca73`，工作区干净。

安装 Smoke PASS 后等待 35 秒才开始强停采样。两轮实际均为 GraphiteDawnVulkan、独立 GPU、crash count=0，恢复 5 标签并完成 30 秒稳定期：

| 轮次 | StartupHealth | 同文档导航耗时 | 导航结束→下一次接受首帧检查 |
|---|---:|---:|---:|
| run1 | 2403 ms | 3.670 ms | 187.182 ms |
| run2 | 2539 ms | 18.203 ms | 232.806 ms |

两轮首个被取消的 frame 工作均明确由 `same_document_start` 失效，而不是恢复后又切换活动标签。活动 WebContents 切换出现在首个 frame 工作接受之前。最终工作均经 visual-state 和成功 presentation 才回包；这里不把 native 的候选捕获序号当作真实文档 ID。

原始记录：`artifacts/startup-invalidation-0ad22e45/run{1,2}/`，采集与分析脚本也保留在该目录。新版采集脚本即使失败也保存错误、实际后端和设备日志，不把失败样本混入性能统计。

### 修复版本 `f8949dd4`

保留同文档导航开始时取消旧视觉/呈现回调的行为，完成导航后投递一次可取消任务，立即重新进入原有 Request 检查路径，重新取当前文档、插入 visual-state callback，再申请成功 presentation。不复用旧 proof，不接受旧 requestId，不在 ready/FCP 提前移除遮罩。

投递任务复用同一 weak-pointer 代际：新请求、跨文档导航、活动标签变化、取消或销毁都会使它失效；连续同文档导航只保留最后一次重试。ready 阶段或没有在途 frame 工作时不新增重试。

`scripts/tests/test-startup-handoff-retry.py` 提取生产实现的四个方法，在最小任务/weak-pointer harness 下验证上述调度和取消路径（不是完整 Chromium 渲染单测）；本机通过，并纳入 Adapter CI。Lumie 全量 165 项测试仍通过。端到端收益需以修复版装机 trace 另行核实，不能直接把表中的 187 / 233 ms 作为必然收益。

### 修复版装机结果

- Adapter CI `39375389952`、native CI `39375389887` 均成功，发布 `build-f8949dd4`；本机完整构建和安装 Smoke PASS。
- HAR SHA256：`1c985599771b5344c01197516608d821bf983cafed348e1313e5568f8d1bee32`。
- HAP SHA256：`4d571976f099af969b0c7931038616ca34d3bc3828f937468fdca5baf9670d3c`。
- HAP/HAR 内 libweb_engine.so 均为 `a2b9f343294f8094427286fac05ecabe5019506e80b0821a261a7b31e22e89bc`，libnweb_render.so 均为 `2f530f9997f2d9682d1736f8aa2db615aae4b37a2a7d7e2923e0703b0c83720a`。
- 安装后仍先等待 35 秒，再开始强停采样。所有三轮均为 GraphiteDawnVulkan、独立 GPU、crash count=0，恢复 5 个标签，完成 30 秒稳定期。最终截图确认页面正常可见，没有启动遮罩。

两轮无受控探针的自然启动：2384 / 2330 ms；对应 GPU 初始化 1.620255 / 1.570099 秒。两轮同文档导航均发生在首个 frame 请求接受之前，**没有触发新重试分支**。因此不能将其与诊断版 2403 / 2539 ms 的差异直接归因于修复；GPU 初始化和网络/页面时序也不同。

第三轮为分支验证，启用了同地址、同状态 `history.replaceState` 探针（隔离执行环境，不新增历史项、返回结果确认 URL 未变）。实际首个受控探针到主机启动后 11908 ms 才执行，启动在此前已确认；trace 中取消在途工作的同文档导航是**受控探针之前的自然加载事件**：

| 事件 | 相对 trace 首事件 |
|---|---:|
| work=2 被 same_document_start 取消 | 929.546 ms |
| same_document_finish | 934.672 ms |
| 自动 retry / same_document_finished | 935.885 ms |
| work=3 新 visual-state 检查被接受 | 935.903 ms |
| work=3 visual-state 成功 | 1074.832 ms |
| work=3 成功呈现并匹配当前文档 | 1508.188 ms |
| native presented 回包 | 1508.206 ms |

导航结束到重试 **1.213 ms**、到接受新检查 **1.231 ms**，确认原先等 250 ms 轮询槽位的局部等待已消除，同时没有复用旧 proof。第三轮 StartupHealth 2412 ms 只作功能记录，因启用额外 CDP 操作，**不纳入性能比较**。

最终装机为 `f8949dd4` native + `fc4ca73` 外壳；Lumie 源码工作区干净。本次两个 native 提交已推送 `hmos-154-adapter`，其余用户已有修改未动。证据在 `artifacts/startup-invalidation-f8949dd4/`，上一版 HAR、缓存和签名 HAP 保留在其中 `previous-engine/`。下一步应继续分别量化首次 frame 请求前的导航等待、visual-state 和成功呈现反馈，不把本次局部消除的等待写成稳定的端到端收益。

### 继续：移除即将关闭的临时标签导航（Lumie `20000c4`）

用户反馈仍慢后，外壳在支持 native handshake、存在待恢复地址、saved-first 且非 recovery 的原生会话恢复路径，不再先导航临时 probe 标签。成功恢复后该标签本来就会关闭。外部拉起、地址恢复、恢复模式和 legacy 路径仍先导航；原生恢复失败按地址兜底时显式初始化第一个保存的网址，避免遗留 probe 页。最终活动文档的 visual-state / successful presentation 门禁未更改。

- 3 个改动 ArkTS 文件检查无错误；Node 24 全量 168 项测试通过；完整构建及装机 Smoke PASS。
- 新 HAP SHA256：`7c9e2e50b1c791db4652829725d3c5b123fc00d6e53c25a9a24b1f16266c7077`；两份 native so 哈希与上节 `f8949dd4` 完全一致。
- 安装后等待稳定，再采自然启动 `run4` / `run5`，证据仍在 `artifacts/startup-invalidation-f8949dd4/`；这两轮的外壳为 `20000c4`，不是前几轮 `fc4ca73`。
- StartupHealth：2421 / 2097 ms；GPU initialization：1.666124 / 1.589516 s；均为独立 GPU、GraphiteDawnVulkan、crash count=0、恢复 5 标签并完成 30 秒稳定期。
- visual-state 等待：235.701 / 568.237 ms；之后 presentation 等待：398.870 / 249.595 ms。这些阶段与 GPU 初始化存在并行，不能把它们直接相加成总启动时间。

两轮结果有明显波动，不能证明稳定端到端提速，更不能称体感慢已解决；提交只确认移除一次无用导航。当前装机 `f8949dd4` native + `20000c4` shell。主瓶颈仍需处理手机 fork GPU 的系统 EGL/IPC 初始化，不能靠放宽呈现门禁、偷偷切换后端或采用非独立 GPU 样本宣称解决。

补充限制：run5 收尾截图显示 Lumie 主页和 5 标签，没有启动遮罩；并非此前的 Google 结果页。尚未确认这是启动选择行为还是采样期间的设备交互，因此两轮也不能当作相同最终页面的严格 A/B，2097 ms 不作为确定提速证据。后续需核对恢复活动标签，勿依据此截图擅自更改用户页面。

## 预热测试版后续：遮罩剩余等待（18:12—18:15）

用户要求继续定位剩余加载提示。保持 native f8949dd4 与 shell 20000c4 +
GameAware 预热候选实现，只临时加入 StartupTiming 日志；未改 250ms
轮询、导航或呈现成功条件。临时 patch 与原始采样在
`artifacts/startup-prewarm-followup-20261020/`。

安装后的无 trace 日志样本：StartupHealth 1492ms；ready 回包到首个 frame
请求 141ms；该请求返回 waiting，198ms 后下一次轮询才重发。此样本没有
原生 trace，不能给其 waiting 指定原因。

两次自然进程冷启动，小类别 startup trace：

| 指标 | run1 / PID 10999 | run2 / PID 11711 |
| --- | ---: | ---: |
| StartupHealth 前台至确认 | 1723ms | 1263ms |
| GPUInfo initializationTime | 194.649ms | 181.637ms |
| 主进程预热总耗时 | 2.695ms | 1.984ms |
| ready 回包→首个 frame 发送 | 209ms | 210ms |
| 最终 frame 接受→visual state | 97.531ms | 161.345ms |
| presentation 注册→成功回调 | 346.357ms | 68.675ms |

均为独立 GPU、GraphiteDawnVulkan、GPU crash count=0，且完成 30 秒
前台稳定期。未启用受控导航或提前查询 GPUInfo；GPUInfo 在启动十秒后才
读取。CDP trace 会扰动启动，run1 Tracing.start 应答约十秒才返回；早期
请求可能未覆盖，不能把这两轮当作严格无扰动 A/B。

重要纠正：三个日志样本均没有触发 LastSessionRestorer 的 start/event/
complete 日志。`startup renderer ready; restoring tabs before presentation`
是通用阶段文案，不能据此说剩余耗时是“完整会话恢复”。这里观察到的是
启动探针页向最终文档切换及呈现确认。

run1 trace 明确显示：ready 回包后，首个 frame 请求仍遇到 probe_document，
原生兜底发起目标导航并回复 waiting；约 32ms 后文档提交，但直到约 222ms
后下一次请求才接受 frame 检查。随后 visual-state 97.531ms，presentation
346.357ms。不存在被同文档导航取消后等轮询的问题；这轮没有该分支。

run2 最终主文档提交到 frame 接受相隔 143.939ms，此间等待下一次轮询；
frame 回调本身仍有 visual-state 和 presentation 两段。不能把 ready→frame
的 210ms 与提交后 144ms 叠加：它们有重叠；也不能宣称事件驱动会把全部
等待按原数值变成端到端收益。

下一步最小修复候选是保留原生完整 proof，令 ready→frame 阶段切换及
最终文档/恢复完成事件立即唤醒检查，以 250ms 定时器作兜底；需要处理导航
pending、probe_document 和 requestId/在途请求合并，而不是只缩短计时器或
提前放开遮罩。当前仅提出候选，尚未实施。已有 main_document 失效事件
并不会像 same_document_finish 修复那样自动重试，需单独验证。

本轮同时核对非手机路径：普通 renderer/utility 用
OH_Ability_StartNativeChildProcess；隔离 renderer 选用
OH_Ability_StartNativeChildProcessWithConfigs；独立 GPU 则使用
OH_Ability_CreateNativeChildProcess 的 Stub/Proxy 和 IPC C API。手机
fork/socket 路径与这套官方原生进程通道不同，Chromium Mojo 也不是系统
Binder 的替代实现。

临时 StartupTiming 七处日志已撤回，相关两个外壳文件与修改前 diff 为空；
重新完整构建和覆盖安装 Smoke PASS，保留 GameAware 预热模块。普通运行
PID 14444：18:15:27.764 warmed total_ms=1.905，18:15:28.936 启动呈现
确认 foreground_ms=1096；这是恢复后功能复核，不据单次结果宣称稳定提速。
无调试器或 CDP tracing 遗留运行，未改变用户数据、图形后端和呈现门禁。
