# 可视区域双语翻译 v1

实现基线 Chromium 154.0.8037.97；首版能力 `frameScope=mainFrame`。手机单进程、手机实验多进程、平板和 2in1 使用相同入口，不改变 GPU/JIT 配置。只支持活动主框架 HTTP(S)、非 opaque origin、`text/html` 文档；不覆盖 iframe（包括同源）、shadow DOM、PDF、图片、字幕、内页、扩展页和 srcdoc。

## 分层与数据

`translation.cc` 为独立 WebContents 服务，Profile 身份由 Profile 自有 user data 持有，普通/无痕隔离。`navigationId` 是原生生成的随机值，绑定 Chromium 公共 `WeakDocumentPtr` 的真实文档生命周期；刷新、同址新文档及 RFH 复用都使旧身份失效，不用 URL 判断文档。同文档跳转保留会话。

`runtime.js` 仅由原生 `ExecuteJavaScriptInIsolatedWorld` 执行，具有独立 `ISOLATED_WORLD_ID_LUMIE_TRANSLATION`。复用 Chromium 既有 browser→renderer 脚本 IPC，不增加通用脚本执行命令、网页消息桥或付费 API 入口；无需自定义 Mojo 接口或复用广告模块。原生再次检查批次、片段数、UTF-16 单位和身份。网页不能调用隔离世界入口，但插入 DOM 的译文不应被宣称对网站保密。

仅用户启动会话后建立观察器。按段收集叶级 p、标题、li、dt/dd、blockquote 及叶级 div/section/article；保留原 DOM。过滤表单及后代、按钮、input/textarea/select、代码/pre、editable、隐藏、translate=no/notranslate、iframe 和本组件节点。不追踪 shadow DOM。父块包含正文子块时只处理子块，父块的散落文本首版不单独翻译。文本空白归一，Unicode 不截断代理对。

内核不联网、不保管服务商配置或密钥、不把正文/译文写日志或磁盘。外壳负责授权、隐私提示、安全存储、Microsoft/Google/AI provider、取消和限流。服务切换须 stop + start；新会话唯一身份使旧结果失效。仅发送文本及源/目标语言给服务商，不发送内核身份、URL、Cookie 或 DOM。

## 正式协议

每条命令必须有整数 `requestId`。事件走现有 runtime event；所有写命令必须等确认。旧 HAR 没有回包时应显示不可用。HAR 导出 `TranslationTypes.ets`，命令通过 `BrowserCommandPayload` / `BrowserCommandChannel`。

| 命令 | 输入 | 回包 |
|---|---|---|
| getTranslationCapabilities | requestId | translationCapabilities：protocolVersion=1、supported、styles、frameScope、limits |
| getTranslationState | requestId, tabId | translationState：当前 contextId/navigationId、会话及无正文计数 |
| startTranslation | requestId, contextId, tabId, navigationId, sourceLanguage（可空）, targetLanguage, style, prefetchViewportRatio（默认 0） | translationSessionResult，原生 sessionId |
| takeTranslationBatch | requestId + 四个会话身份，maxSegments（1–32）, maxTextUnits（2000–12000） | translationBatch，segments、非空时 batchId/leaseMs，hasMore |
| applyTranslationBatch | requestId + 会话身份，batchId，segments | translationBatchResult，applied/rejected/results |
| releaseTranslationBatch | requestId + 会话身份，batchId，reason，retryable | translationBatchResult |
| setTranslationPresentation | requestId + 会话身份，style，showTranslation | translationSessionResult |
| stopTranslation | requestId + 会话身份 | translationSessionResult，同一已停止会话幂等 |

会话身份是 `contextId,tabId,navigationId,sessionId`。片段包含 `frameId,frameDocumentId,segmentId,segmentRevision,text,priority`。首版仅主框架，两个 frame 字段均使用当前文档身份，未来不能推导为全框架 ID。写回保留所有身份，将 text 替换为 translatedText。未知/重复 ID、错误 frame/revision、超大输入在原生层整批拒绝；合法批次内源 DOM 已变可部分应用，results 按 segmentId/revision 给固定原因。

`applied` 表示通过当前版本验证并接受结果；DOM 插入在后续分片任务中完成，插入前再次校验源文。同一段落所有片段齐备才显示组合译文；部分片段失败不展示半段。`translated` 统计已接受片段，不代表整篇文章或所有段落已经显示。`discovered` 是累计创建的内容版本片段数，包含因动态变化失效的版本；`inFlight/translated/failed` 为当前映射状态。

异步事件：translationStateChanged、translationWorkAvailable（无正文，变化时合并）、translationBatchInvalidated、translationSessionEnded。session 内 sequence 单调递增；查询 state 不推进序号。只在 take 回包中出现源文；其他通知不含正文。外壳不要完全依赖通知：恢复前台、批次结束、收到 busy 时在 300–500 ms 后查询/拉取。take 空数组成功表示当前没工作；不可当成整篇翻译结束。

状态包括 idle/waiting/translating/paused/error/stopped。release reason 只允许 providerError/rateLimited/authentication/quota/cancelled/timeout。鉴权/额度失败将会话置 error 并暂停派发，用户处理后重新 start；不会自动换 provider。其他可重试释放有 2/4/8 秒退避，每片最多 3 次领取。429 外壳须按服务商 Retry-After 延后拉取，内核不解析服务商响应。

固定失败码包括 invalidRequest/invalidTab/unsupportedPage/staleDocument/sessionEnded/busy/paused/concurrencyLimit/batchExpired/invalidResult/tooLarge/staleSegment/rendererGone/rendererTimeout/limitReached。发送成功不是执行成功；busy 时重发同一操作需新 requestId。未知/过期/已结算 batchId 不得重放。原生脚本操作超时 5 秒会结束会话、发失败确认，并拒绝迟到回调；批次租期 45 秒，外壳网络超时应更短。

## 调度与内存边界

- 最多 2 个批次在途；每批 32 片/12,000 UTF-16 units，每片 2,000；结果每片最多 8,000、每批 256,000。源文分片不能切断 Unicode 代理对。maxTextUnits 最小 2000，确保任一片可被取走。
- 默认不预取；可配置下方 0–0.5 个视口，当前可视内容优先；滚动停稳 180 ms 后派发。不取消仍有效的在途任务，不累计沿途屏外待发请求。
- 发现分片、IO 可见性、有限 mutation 根队列，不在每次 scroll 时全树扫描。节点发现可以遍历屏外结构，但不读取/返回屏外正文。已看过内容仅当前会话内复用。
- 最多 5000 个被跟踪段落节点、累计 5000 个内容版本片段、128 个待发现根。累计源文+接受的结果最多 1,048,576 UTF-16 units（逻辑文本 2 MiB；不等于包含 DOM/V8/索引的总进程内存）。达到上限保留已有译文，停止新增，无静默淘汰后重复计费。原生对交付过的源文和提交结果也有保守预算。
- 发现/插入循环 4 ms / 64 个节点操作后让出，take 也限制循环预算。单次 TreeWalker/布局/选择器/文本读取、排序、统计、切样式及 stop 清理不可抢占，4 ms 是目标而非已证明的硬上限；长任务和真机数据仍须测量。
- 标准/引用/灰背景只改现存节点样式，使用 closed shadow root 与静态 constructable stylesheet，纯文本节点写入；不修改站点 CSP/Trusted Types。lang 使用目标语言，dir=auto，quote 使用逻辑方向；gray 使用 light-dark 跟随主题。不开启额外网络请求。
- 显示原文隐藏译文并暂停派发；迟到的有效结果仍可缓存。关闭会话移除节点和 observer/timer。页面修改原文或复用节点使版本失效，旧结果不能插入。

导航开始即结束会话并清理旧 DOM；因此进入 BFCache 的文档不保留活动翻译会话，返回后需用户重新启动（首版明确采用取消策略）。同文档 hash 保留；SPA 通过 DOM mutation 失效源版本。后台/非当前标签暂停；预渲染不启动。关闭标签、renderer 退出、Profile 销毁终止任务。异步响应始终绑定原 tab，不使用“当前标签”替代目标。应用重启不恢复正文和在途请求。

## 验证与外壳接入

宿主测试运行实际隔离世界 JS，不是原生 IPC/鸿蒙验收。可运行：

```sh
PLAYWRIGHT_MODULE=/path/to/playwright/index.mjs CHROME_PATH=/path/to/chrome node scripts/tests/translation-browser.mjs
```

合成页面：`docs/test-pages/viewport-translation/index.html`。用户自行通过已有测试服务器发布，或用 `python3 -m http.server 8110 --directory docs/test-pages`。不自动操作手机，不接真实服务。

外壳先接确定性假 provider：把每片译文设为 `【测试译文】` + text；提供延迟 0/2/10/50 秒、逆序片段、丢弃回包、超大结果、鉴权/额度错误开关。只用合成页面，验证 UI 和实际 native 命令。顺序是 capabilities → getState(tabId) → start(返回身份) → take → apply/release；每批确认后再拉。样式不经过 provider。

验收应补齐：同址刷新/跨站跳转/标签关闭/多窗口移动的迟到结果；45 秒过期回收和最多 3 次领取；DOM 变更部分成功；非法 frame/segment/revision；后台停止新请求；单进程/多进程/平板；普通/无痕隔离；原链接/表单/选择；严格 CSP、RTL、主题/字号/窄屏；长文章/动态列表的采集与插入 p95、长任务和总内存。真机结果未取得前不能称全部验收通过。
