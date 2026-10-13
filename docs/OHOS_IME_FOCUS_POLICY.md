# 网页自动聚焦不应唤起键盘

2026-10-13：用户反馈 BrowserBench 部分子测试反复唤起键盘。源码中存在两个独立触发点：

- Blink 的 `WidgetBase::ShowVirtualKeyboardOnElementFocus()` 在 OHOS 默认调用
  `ShowVirtualKeyboard()`，DOM `focus()` 因而也会发送显示请求。
- `OhosInputMethod::Attach()` 根据 `GetFocusReason()` 或全局最近 1 秒触摸记录
  决定显示键盘。Aura 的 focus reason 来自整个网页 view 的旧 pointer focus，
  不能证明新聚焦的 DOM 元素是由用户点选的。

修复不按 BrowserBench 域名判断：OHOS 的元素焦点变化只同步输入状态，输入法
attach 时不主动展示键盘。真正的显示请求继续由 Blink `DidHandleGestureEvent`
的 tap/long-press 路径和原生 Views 文本框发起；已经聚焦的输入框再次被点击也有效。
失焦、不可编辑或 `inputmode=none` 的客户端不接受显示请求。单进程和多进程共用此策略。
ArkUI 外壳地址栏不经过该 native IME，未修改外壳。

## 真机验收

用 `scripts/serve-test-pages.sh`，访问 `http://localhost:8080/keyboard-focus.html`：

1. 冷启动导航到页面，`autofocus` 不应唤起键盘。
2. 键盘收起时启动自动焦点测试：40 次 input / textarea / contenteditable / iframe
   聚焦、失焦，键盘不应跳出，页面布局应稳定。页面的视口计数不是系统键盘检测。
3. 手指点文本框正常显示键盘；用系统返回收起后，再点同一框应重新显示。
4. 数字输入、中文组合与候选、删除、回车、多行、iframe、长按编辑均正常。
   `inputmode=none` 不唤起软键盘；硬件键盘仍可输入。
5. 再运行用户复现的 BrowserBench 子测试和完整一轮测试，不应反复弹出键盘。
6. 单进程及实验多进程分别回归；检查原生 Views 搜索框、外壳地址栏、登录和自动填充。

源码检查和构建通过不能代替上述真机验收，当前尚未复测用户的 BrowserBench 场景。
