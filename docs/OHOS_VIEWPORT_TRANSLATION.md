# 可视区域双语翻译 v1

实现基线 Chromium 154.0.8037.97；首版能力 `frameScope=mainFrame`。手机单进程、手机实验多进程、平板和 2in1 使用相同入口，不改变 GPU/JIT 配置。只支持活动主框架 HTTP(S)、非 opaque origin、`text/html` 文档；不覆盖 iframe（包括同源）、shadow DOM、PDF、图片、字幕、内页、扩展页和 srcdoc。

## 分层与数据

`translation.cc` 为独立 WebContents 服务，Profile 身份由 Profile 自有 user data 持有，普通/无痕隔离。`navigationId` 是原生生成的随机值，绑定 Chromium 公共 `WeakDocumentPtr` 的真实文档生命周期；刷新、同址新文档及 RFH 复用都使旧身份失效，不用 URL 判断文档。同文档跳转保留会话。

`runtime.js` 仅由原生 `ExecuteJavaScriptInIsolatedWorld` 执行，具有独立 `ISOLATED_WORLD_ID_LUMIE_TRANSLATION`。复用 Chromium 既有 browser→renderer 脚本 IPC，不增加通用脚本执行命令、网页消息桥或付费 API 入口；无需自定义 Mojo 接口或复用广告模块。启动采用两阶段文档标记：第一阶段只设置隔离世界标记，不读取正文；原生确认 WeakDocumentPtr 仍有效后才启动执行器，第二阶段及后续操作还必须匹配原文档标记，避免导航与脚本 IPC 竞争时把会话启动到新文档。原生再次检查批次、片段数、UTF-16 单位和身份。网页不能调用隔离世界入口，但插入 DOM 的译文不应被宣称对网站保密。

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

可见段落被发现后，在原文下方插入本地 CSS 渐变文字占位“正在翻译...”；提示使用应用界面语言（中、英、日、韩、法、德、西、葡、俄、阿，繁中单独处理，未知语言回退英语），与译文目标语言分开。完整译文准备好后替换占位，不显示半段结果。终止失败、鉴权/额度错误、原文变更和关闭会话会移除对应加载占位。显示原文时隐藏占位；遵循 prefers-reduced-motion，不请求 GIF、图片或其他外部资源。引用样式在译文外侧使用 4px currentColor 逻辑起始边线，跟随文本方向及继承颜色。

## 内容识别与结构化插入（2026-10-21，待原生 HAR 构建）

- 使用自主实现的通用规则和本地站点选择器，不引入沉浸式翻译的扩展运行时或复制旧版源代码。参考其公开文档中的范围/排除/站点覆盖分层思路：https://immersivetranslate.com/docs/advanced/ 。
- 跳过表单、编辑区、代码/快捷键/公式、导航、工具栏、菜单、按钮角色、时间、页脚及显式禁止翻译/隐藏节点。普通页头不整体排除，避免漏掉文章标题；不猜测任意 class 是否代表用户名或广告。
- 已是目标语言的逐段跳过是保守的本地快速路径，而非完整语言识别模型：在可见相邻文本片段分片、创建 loading 和发送请求前判断，不使用整页或元素 `lang` 决定跳过。至少 8 个文字字符，超过 8000 单位的片段保留翻译以限制同步判断开销；中文需要常见中文短语证据，显式简繁目标只接受小型封闭字表中的不变字/对应字形，未知字及一对多转换继续翻译；通用 zh 不做简繁转换。日文需要每句假名证据，韩/希腊/亚美尼亚/格鲁吉亚/泰/老挝/高棉文使用保守文字范围判断。显式非默认书写体系仍保留翻译。
- 任意外文词（包括 API 等术语）会阻止中文等纯文字规则跳过；短句、混合语言和未知内容照常翻译，不按“目标文字占比高”就丢弃整段。保护的代码/公式不参与正文语言判断。英语/法语等拉丁文字语言，以及共享西里尔/阿拉伯文字的语言，目前没有可靠本地分类器，仍照常请求；不声称已覆盖所有目标语言。相同文字范围内部的混合语言仍有启发式局限，后续可接入本地统计识别器提高覆盖率。
- 跳过片段不保留正文、不占累计文本预算、不产生重复译文或 loading；文字变化重新判断，已在途的旧文字结果仍按版本失效；切换目标语言重启会话后重新判断。整个判断不额外联网，不上传正文做语言识别。
- 同一容器的直接文本和行内元素按相邻片段分组，遇到嵌套段落分界，避免漏掉父容器的前后文字或重复上传父子全文。混合容器按片段 Range 几何判断视口，不因为整个 article 可见而发送屏外尾文。
- 普通叶子段落仍在元素后插入译文；列表项、定义项、表格单元格/标题、图片说明、summary 的译文保留在原语义容器内部。混合容器在对应片段后插入，保持阅读顺序；译文继承源元素字体、颜色、对齐，保留 standard/quote/gray 选择及 RTL 逻辑边线。
- 初始内置适配为 GitHub Markdown/问题标题、Hacker News 标题/评论、X/Twitter 推文/简介、YouTube 描述/评论。仅在启动时发现已知内容选择器才启用站点范围，否则回退通用规则；这些选择器需要持续维护，不代表已通过全部真实站点验收。
- 行内 code/kbd/samp/var 和 MathML 公式在请求中替换为本地占位符；保护内容本身不发给服务商。译文按标记位置重建纯文本代码及白名单 MathML 结构，不复制原属性、事件、脚本或任意 HTML。每片标记的集合和次数在外壳入缓存前及 renderer 写入前分别校验；丢失/重复/新增均拒绝。分片不切开生成标记，原文中的同形标记通过命名空间避碰；本地保护内容变更也使旧批次失效。
- 每个段落片段最多 64 个保护对象，每对象最多 256 个节点、32 层、8000 文本单位；保护结构的序列化长度计入保守累计预算。不宣称完整保留原富文本链接样式，也未覆盖 KaTeX/MathJax 的所有 HTML/SVG 公式。图片 OCR、跨 frame、PDF 仍不在这一版范围。
- Flex/Grid 子项的译文留在子项内；布局容器本身的直接文本另行处理，避免重复提交子项。横向 Flex 的直接文本译文占完整新行，临时 wrap 会在隐藏或结束时撤销。Grid 直接文本译文跨该容器列；不调整网站原有网格列定义。
- 固定高度/裁剪祖先只按译文所占高度增加 height/max-height，保留 overflow 裁剪；叶子段落自身截断则为完整原文及译文保留高度，并暂时解除其 line-clamp。overflow:auto/scroll 容器保持原视口和滚动行为。这是常见正常流布局的适配，复杂绝对定位/变换布局仍需站点测试，不承诺任意页面零变化。
- 布局属性按多段共享所有权保存；隐藏/结束/失效时恢复原内联值及 important 优先级，且不覆盖网页随后对该属性的修改。布局更新分批执行，不在样式切换命令中同步重排所有段落。
- 页面修改文字、拆换片段或将祖先改为排除区域时，撤销旧版本并清理译文。停止翻译移除插入节点并恢复受管布局属性；不包裹、克隆或改写原文文本。
- Lumie 外壳新增每个 TranslationJob 独立的原文→译文 Map：同批去重、跨批命中复用，恢复各原片段身份与顺序。最多 5000 项/1,048,576 UTF-16 units（原文加译文）；超限结果仍正常返回但不缓存。停止/关闭/文档会话结束或不可恢复错误清空，不落盘，不跨标签、目标语言、服务商或模型共享；单纯隐藏译文保留缓存。

异步事件：translationStateChanged、translationWorkAvailable（无正文，变化时合并）、translationBatchInvalidated、translationSessionEnded。session 内 sequence 单调递增；查询 state 不推进序号。只在 take 回包中出现源文；其他通知不含正文。外壳不要完全依赖通知：恢复前台、批次结束、收到 busy 时在 300–500 ms 后查询/拉取。take 空数组成功表示当前没工作；不可当成整篇翻译结束。

状态包括 idle/waiting/translating/paused/error/stopped。release reason 只允许 providerError/rateLimited/authentication/quota/cancelled/timeout。鉴权/额度失败将会话置 error 并暂停派发，用户处理后重新 start；不会自动换 provider。其他可重试释放有 2/4/8 秒退避，每片最多 3 次领取。429 外壳须按服务商 Retry-After 延后拉取，内核不解析服务商响应。

固定失败码包括 notReady/invalidRequest/invalidTab/unsupportedPage/staleDocument/sessionEnded/busy/paused/concurrencyLimit/batchExpired/invalidResult/tooLarge/staleSegment/rendererGone/rendererTimeout/limitReached。发送成功不是执行成功；busy 时重发同一操作需新 requestId。未知/过期/已结算 batchId 不得重放。原生脚本操作超时 5 秒会结束会话、发失败确认，并拒绝迟到回调；批次租期 45 秒，外壳网络超时应更短。

## 调度与内存边界

- 最多 2 个批次在途；每批 32 片/12,000 UTF-16 units，每片 2,000；结果每片最多 8,000、每批 256,000。源文分片不能切断 Unicode 代理对。maxTextUnits 最小 2000，确保任一片可被取走。
- 默认不预取；可配置下方 0–0.5 个视口，当前可视内容优先；滚动停稳 180 ms 后派发。不取消仍有效的在途任务，不累计沿途屏外待发请求。
- 发现分片、IO 可见性、有限 mutation 根队列，不在每次 scroll 时全树扫描。结构发现会检查直接文本是否全为空白，但不会拼装/派发屏外片段正文；片段正文提取受视口及可选预取范围约束。已看过内容仅当前会话内复用。
- 最多 5000 个被跟踪段落节点、累计 5000 个内容版本片段、128 个待发现根。累计源文+接受的结果最多 1,048,576 UTF-16 units（逻辑文本 2 MiB；不等于包含 DOM/V8/索引的总进程内存）。达到上限保留已有译文，停止新增，无静默淘汰后重复计费。原生对交付过的源文和提交结果也有保守预算。
- 发现/插入循环 4 ms / 64 个节点操作后让出，take 也限制循环预算。单次 TreeWalker/布局/选择器/文本读取、排序、统计、切样式及 stop 清理不可抢占，4 ms 是目标而非已证明的硬上限；长任务和真机数据仍须测量。
- 标准/引用/灰背景只改现存节点样式，使用 closed shadow root 与静态 constructable stylesheet，纯文本节点写入；不修改站点 CSP/Trusted Types。lang 使用目标语言，dir=auto，quote 使用逻辑方向；gray 使用 light-dark 跟随主题。不开启额外网络请求。
- 显示原文隐藏译文并暂停派发；迟到的有效结果仍可缓存。关闭会话先在原生层撤销批次和会话授权，再等待 renderer 清理确认；超过 5 秒回报 rendererTimeout，不能把发送成功当清理成功。重复停止会重新确认幂等清理。关闭会话移除节点和 observer/timer。页面修改原文或复用节点使版本失效，旧结果不能插入。

导航开始即结束会话并清理旧 DOM；因此进入 BFCache 的文档不保留活动翻译会话，返回后需用户重新启动（首版明确采用取消策略）。同文档 hash 保留；SPA 通过 DOM mutation 失效源版本。后台/非当前标签暂停；预渲染不启动。关闭标签、renderer 退出、Profile 销毁终止任务。异步响应始终绑定原 tab，不使用“当前标签”替代目标。应用重启不恢复正文和在途请求。

## 验证与外壳接入

宿主测试运行实际隔离世界 JS，不是原生 IPC/鸿蒙验收。可运行：

```sh
PLAYWRIGHT_MODULE=/path/to/playwright/index.mjs CHROME_PATH=/path/to/chrome node scripts/tests/translation-browser.mjs
```

合成页面：`docs/test-pages/viewport-translation/index.html`。用户自行通过已有测试服务器发布，或用 `python3 -m http.server 8110 --directory docs/test-pages`。不自动操作手机，不接真实服务。

可将 `docs/tools/ViewportTranslationProbe.ets` 复制到外壳调试模块，构造时传入 `BrowserCommandChannel.send` 包装函数，将 runtime event 交给 `onEvent()`；由调试按钮调用 start/pump/presentation/stop。该样例没有联网能力，不能作为自动启用的正式 provider。

外壳先接确定性假 provider：把每片译文设为 `【测试译文】` + text；提供延迟 0/2/10/50 秒、逆序片段、丢弃回包、超大结果、鉴权/额度错误开关。只用合成页面，验证 UI 和实际 native 命令。顺序是 capabilities → getState(tabId) → start(返回身份) → take → apply/release；每批确认后再拉。样式不经过 provider。

验收应补齐：同址刷新/跨站跳转/标签关闭/多窗口移动的迟到结果；45 秒过期回收和最多 3 次领取；DOM 变更部分成功；非法 frame/segment/revision；后台停止新请求；单进程/多进程/平板；普通/无痕隔离；原链接/表单/选择；严格 CSP、RTL、主题/字号/窄屏；长文章/动态列表的采集与插入 p95、长任务和总内存。真机结果未取得前不能称全部验收通过。

## 2026-10-16 自动验证记录

- 宿主 Chrome 155.0.8059.82 的实际隔离世界执行器：28 项通过，涵盖滚动、溢出裁剪、嵌套滚动、缓存、样式、敏感区域、严格 CSP/Trusted Types、Unicode 分片、动态内容、租期、重试、乱序、超大结果和上限。租期/退避测试使用可控宿主时钟，不等待真实 45 秒。
- HAR 类型及外壳假 provider 样例：定向 ArkTS 检查 4 个文件无错误。全项目扫描仍有 entry 模块现存的 11 个 engine 导出解析错误，不能把定向检查称为全项目检查通过；最终 HAR 打包结果另行核对。
- macOS arm64、480×800 视口、4500 段合成正文、20 轮滚动：20 批成功应用；take p95 0.9 ms、apply p95 0.3 ms，执行器定时任务 p95 0.3 ms、最大 0.7 ms，未记录到 >50 ms 长任务。样本少且带测试计时开销，不构成手机 120 Hz 帧预算保证；一次性总内存、页面 DOM 清理和真实手机 p95 仍待验收。
- 标准/引用/灰背景、深色截图由宿主受控页面生成，不是真机截图；原生 IPC、手机/平板生命周期验收仍待用户完成。

## 2026-10-21 加载占位与引用边线（源码验证，尚未交付 HAR）

- Chrome 155.0.8059.85 实际隔离世界执行器 31 项通过，新增加载动画、减少动态效果、加载替换、终止失败及鉴权失败清理检查，并检查加载/完成状态截图。
- 4500 段合成页面的 20 轮滚动全部取得并应用批次；take p95 0.9ms、apply p95 0.2ms，未记录到 >50ms 长任务。该结果仅为宿主测试，不代表鸿蒙真机性能。
- 改动位于内嵌 renderer runtime，需要重新编译内核及打包 HAR 后才能进入手机；本轮没有操作手机、提交或触发远端构建。

## 2026-10-21 内容规则、渐变提示与页面缓存（本地验证）

- Chrome 155.0.8059.85 实际隔离世界测试 40 项通过；新增混合段落顺序/动态更新/片段视口、语义列表表格、交互区域排除与撤销、站点范围/回退、标题字号及主题更新、多语言加载提示。受控截图已检查，非真实站点全面验收。
- 4500 段、20 轮滚动全部成功应用；take p95 0.7ms、apply p95 0.2ms；调度任务 p95 4.2ms、最大 5.4ms，未记录到 >50ms 长任务。仅宿主 macOS Chrome，不代表手机帧耗时。
- Lumie 的 Node 测试 204 项通过（含缓存和设置试验性功能入口）；5 个变更 ArkTS 文件定向检查无错误，仍有依赖警告；entry 与 entry_wide 本地构建均成功。
- 外壳构建仍链接已有 engine.har，不包含这里的新 renderer/native 修改。新内容规则、渐变文字及 uiLanguage 传递需要后续编译新版内核/HAR；本轮未提交、推送、触发远端构建或安装手机。

## 2026-10-21 占位保护与特殊布局补充验证

- 宿主隔离世界测试 51 项通过；新增代码/MathML 本地还原、危险属性剥离、标记缺失/重复/新增拒绝、保护内容版本变化、分片边界及文字标记避碰、Flex/Grid 子项与直接文字、裁剪/限高/line-clamp、隐藏/结束恢复及网站后续样式修改保留。已检查代码和公式的行内截图。
- Lumie Node 测试 208 项通过；2 个变更 ArkTS 文件检查无错误（仍有依赖警告），entry 与 entry_wide 本地构建成功。
- 4500 段、20 轮滚动全部成功；take p95 0.9ms、apply p95 0.2ms、presentation p95 0.4ms，调度任务 p95 4.5ms、最大 6.5ms，无 >50ms 长任务。仅合成宿主数据，不是手机性能或所有真实网站验收。
- 新 renderer 仍待内核/HAR 编译。本轮没有提交、推送、远端构建、真实翻译服务请求或手机操作。

## 2026-10-21 逐段目标语言跳过验证

- 宿主 Chrome 隔离世界共 58 项通过。新增同页混合语言/错误 lang、简繁目标、日/韩/希腊/泰文、短句及共享文字保守回退、显式书写体系、动态修改与迟到结果、代码保护、切换目标语言、长混合段落先判断再分片、超长段落限制判断开销。
- 4500 段、20 轮滚动全部成功；take p95 0.6ms、apply p95 0.2ms，调度任务最大 5.8ms，未记录到 >50ms 长任务。仍仅宿主合成测试，不是鸿蒙真机验收。
- 本轮仅改 renderer JS、宿主测试及本文档；未改 ArkTS，没有发起真实服务请求、提交、推送、编译原生 HAR 或操作手机。需要新版内核/HAR 才会生效。

## 原生交付（2026-10-16）

- 内核提交 `e193164763428a5161fcbda54e99f465e1cae58d`，原分支已推送，无分支切换。
- Chromium 154 编译、链接、HAR 打包及流水线原生回归通过：[构建 38860977308](https://github.com/miramira8295/chromium-hmos/actions/runs/38860977308)。
- [Adapter CI 38860976942](https://github.com/miramira8295/chromium-hmos/actions/runs/38860976942) 通过。
- HAR SHA256：`f1ad21b2e198facbcc0143c7d2f068f8ec96c3540cedd1e3cc4c3c42b29c894e`。
- 引擎 Build ID：`d93473b27a188d2ce183b733f1f0c772594c81d0`。
- 本地交付目录：`Lumie-viewport-translation-e1931647/engine.har`；同目录 TESTING.md、PROTOCOL.md、假 provider、合成页面、截图和 evidence.zip（不入库）。
- 已验证 HAR 内 8 个原生命令、文档握手标记、TranslationTypes/NativeContext 声明和导出；二进制内嵌执行器与实际测试源码逐字节一致。
- 真机未操作，手机/平板与真实 provider 验收仍待用户完成。文档后续提交不改变交付内核的 commit。
