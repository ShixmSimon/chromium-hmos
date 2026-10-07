# 手机键盘避让与自动填充建议框：交接方案

状态：2026-10-08，两个问题均未解决，以下为已查实的事实、待查假设和实施方案。真机验收由协调者执行，实施 agent 不操作手机。

## 现状与已查实的事实

### 键盘避让的链路（手机）

1. 外壳（Lumie `WindowController`）收到 `keyboardHeightChange`，换算成 vp 写入 `env.keyboardHeight`。
2. `PhoneControlsSync.reportInsets()` 调 `setBottomInset(keyboardHeight)`，发送 `setViewportInsets { bottom }`；同时把底栏（dock）的 browser controls 设为 0。
3. 内核 `aura_shell_runtime_bridge.cc` 的 `setViewportInsets` 存入 `viewport_bottom_inset[widget]`，调 `ApplyViewportInsets()` → `RenderWidgetHostView::SetInsets()`。浏览器状态轮询（约 200 ms）也会对当前标签页反复调 `ApplyViewportInsets()`。
4. `RenderWidgetHostViewAura::SetInsets()` 只在值变化时分配新 LocalSurfaceId 并 `SynchronizeVisualProperties()`；在 `ohos-browser-controls.patch` 里，`GetRequestedRendererSizeDevicePx()` 会把顶栏和 insets 从布局高度里减掉（`GetOhosReservedHeight().reserves_insets`）。
5. 新尺寸的帧激活后，Aura 自己会调一次 `ScrollFocusedEditableNodeIntoView()`（`OnRenderFrameMetadataChangedAfterActivation` 里的 `inset_surface_id_`）。

另外，ArkUI 默认的键盘安全区会把网页的 XComponent 缩到键盘上方（uitest 看到 `[0,137][1316,1733]`），但 Chromium 的布局尺寸不随它变，只看 insets。试过给网页区域加 `expandSafeArea([SafeAreaType.KEYBOARD])`，XComponent 仍被缩小，**无效，已撤回**。

### 问题 A：键盘弹出后网页有时没缩小（输入框被键盘挡住）

- 真机（Mate，手机单进程，`build-2cdee5ab`），ai.biuwk.com 登录页点邮箱框，6 轮中 2 轮失败，失败时网页 `innerHeight`、`visualViewport.height` 一直是 646（正常应为 400）。
- 失败轮里内核日志照常出现 `OHOS viewport inset: bottom=314 view=… visible=378x332`：浏览器侧视图已带上 insets，`GetVisibleViewportSize()` 也是缩小后的值。
- 提交 2cdee5a 在轮询里加了 `RenderWidgetHost::SynchronizeVisualProperties()` 补发，并在它返回 true 时打 `OHOS viewport inset: resent`。失败轮里**一次都没有补发**：浏览器认为渲染进程已经拿到最新的尺寸（`StoredVisualPropertiesNeedsUpdate` 为假），或者同步被前置条件挡住（返回 false 时分不清是哪一种）。
- 现象以前是“重启后第一次正常、之后一直坏”，这次是“前两轮坏、后四轮好”，更像与页面刚加载完（每轮都是新导航，view 指针每轮都不同）的时机有关。

### 问题 B：建议框被键盘盖住 / 键盘升起后消失

- 点输入框后，键盘还没上来，渲染进程先做一次 `ScrollFocusedEditableElementIntoView`，`DidCompleteFocusChangeInFrame` 把建议框弹在当时输入框的下方。
- 键盘上来后网页被推上去，建议框若还开着，会留在旧位置、大部分被键盘挡住（问题 A 发生时更明显，uitest 看到建议框窗口只剩 18～78 像素露在键盘上沿）。
- 已做的修复：
  - `ohos-autofill-follows-keyboard.patch`（7266fab）：第二次 `DidCompleteFocusChangeInFrame` 时，若建议框还开着，就当作点击重新请求，按新位置重画。
  - `ohos-autofill-popup-above-keyboard.patch`（ccf87a6）：`PopupBaseView::GetContentAreaBounds()` 在 OHOS 上把高度限到 `GetVisibleViewportSize()`，下方放不下时翻到输入框上方。
- 真机结果：网页正确缩小的轮次里，建议框**直接消失了**，没有在新位置重新出现。原因：`focus_requires_scroll` 为真时，`AutofillAgent::DidChangeScrollOffset()` 会 `HidePopup()`，`is_popup_possibly_visible_` 变为假，第二次 `DidCompleteFocusChangeInFrame` 就不再重新请求。用户得再点一次输入框。

### 不能做的事（踩过的坑）

- **不要缩小 `WebContentsViewAura::GetContainerBounds()` 的高度。** 网页自身尺寸是按它算的，缩小后两边互相压缩，手机上只剩半个网页（e417008，已在 d4f1e7c 撤回）。
- 不要靠 ArkUI 的键盘安全区来避让网页：Chromium 不按 XComponent 的尺寸排版。

## 实施方案

### A. 先定位，再修：加诊断日志（只记数字）

在一次 CI 里加齐，真机复现 6～10 轮后对照日志：

1. 浏览器侧，`content/browser/renderer_host/render_widget_host_impl.cc` 的 `SynchronizeVisualProperties(bool, bool)`：OHOS 上若带非空 insets 的视图（可在 `RenderWidgetHostViewAura::SetInsets` 里打标）调用时提前返回，记录是哪一个条件（主框架未激活 / 尚未创建 / ack 未回 / 不可同步 / 属性未变），以及发出去的 `new_size_device_px`、`visible_viewport_size_device_px`、LocalSurfaceId。限频：每个视图每次 insets 变化只记一次。
2. 渲染侧，`third_party/blink/renderer/platform/widget/widget_base.cc` 的 `UpdateVisualProperties`（或 `WebFrameWidgetImpl::ApplyVisualProperties`）：OHOS 上记录收到的 `new_size`、`visible_viewport_size`、`browser_controls_params`、LocalSurfaceId，以及是否因为 `IsForProvisionalFrame` / 尚未初始化等原因丢弃。
3. 对照失败轮：浏览器是否真的发出了缩小后的尺寸；渲染侧是否收到；收到后布局是否用了它（例如被 browser controls 的计算覆盖，`DoBrowserControlsShrinkRendererSize` 状态与 `controls_px` 是否在新页面上与浏览器一致）。

注意 `render_widget_host_view_aura.cc` 的 `SetInsets` 紧挨着 `ohos-browser-controls.patch` 的修改段（约 1100 行），在那里加代码会违反“同一文件不同补丁修改段不能相邻”的规则；诊断优先放在 `render_widget_host_impl.cc` 和渲染侧。

可能的修法（按日志选，**不要同时上**）：

- 若浏览器被前置条件挡住、事后没再同步：在 insets 变化而同步失败时，记住“需要补发”，在 `RenderWidgetHostImpl::DidUpdateVisualProperties` / `WasShown` / 页面激活时补发；或在 OHOS 上对键盘 insets 用 `SynchronizeVisualPropertiesIgnoringPendingAck()`（需评估对 LocalSurfaceId 的影响）。
- 若浏览器发出的尺寸本身没缩小（新页面的 browser controls 状态未就绪导致 `reserves_insets` 为假）：修 `GetOhosReservedHeight()` 的判定，insets 的扣减不应依赖 controls 是否已初始化。
- 若渲染侧收到但没用：在渲染侧查 browser controls 与 visual viewport 的计算。

### B. 建议框在键盘升起后重新出现在正确位置

在 `components/autofill/content/renderer/autofill_agent.cc`（现有补丁 `ohos-autofill-follows-keyboard.patch`，修改它并重新生成）：

1. OHOS 上，`DidChangeScrollOffset()` 因 `focus_requires_scroll` 收起建议框时，若建议框原先是开着的，记下 `ohos_reshow_after_scroll_` 和当时聚焦的元素。
2. 第二次 `DidCompleteFocusChangeInFrame()` 时，若该标记为真且聚焦元素没变，就按点击重新请求建议，然后清掉标记。聚焦变化、导航、`HidePopup` 由用户操作触发时清标记。
3. 和 `ohos-autofill-popup-above-keyboard.patch` 一起，重新出现的建议框会在输入框下方（放得下时）或上方（放不下时），不进键盘区域。

B 依赖 A：网页没缩小时，B 也不会触发（不会滚动）。可以先做 B，验收时只看网页正确缩小的轮次。

## 工程规范

- 修改上游文件一律用增量补丁：先**只读**取得 runner 当前文件（`root@192.168.8.110:/root/chromium-154/src`），改完用 `git diff --no-index` 生成；在本地隔离目录模拟 `apply_incremental_patch` 的首次应用、重复应用和旧版升级。改已有补丁时，要验证 CI 能撤掉 runner 上的旧版。**不能手写补丁。**同一文件不同补丁的修改段不能相邻。
- 新增补丁登记到 `scripts/ci-incremental-build.sh` 末尾。只在 CI 编译。
- 日志只记数字和状态，不记网页内容、网址（用 `UrlForLog()`）。
- 同一时间只有一个 agent 推送 `hmos-154-adapter`；推送前先拉取。
- Lumi（外壳）不改；需要外壳配合时写说明交协调者。

## 真机验收（协调者执行）

页面：ai.biuwk.com 登录页（有已保存的账号，会出密码建议），以及测试服务器上的 `ac.html`（表单历史建议）和 `login.html`。

每轮：导航到登录页 → 等 4 秒 → 失焦、滚到顶 → `uitest uiInput click 600 1830` 点邮箱框 → 等 3 秒 → 用 CDP 读**可见标签页**（`document.visibilityState === 'visible'`）的 `innerHeight`，dumpLayout 看 XComponent 和 `aura_aux_*` 建议框窗口的位置，截图 → 返回键收起键盘。奇数轮之间插一次导航到别的页面。至少 10 轮。

通过标准：

1. 每一轮键盘弹出后网页可视区都缩到键盘上方（本机为 400 CSS 像素），输入框可见。
2. 建议框完整显示在键盘上方（输入框下方或上方），不被键盘遮住，不留在旧位置，不需要再点一次输入框。
3. 收起键盘后没有残留的建议框窗口。
4. 地址栏等外壳自己的输入框不受影响；平板 / PC（多进程、不发 insets）键盘行为不变。
