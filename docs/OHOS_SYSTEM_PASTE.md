# 跨应用纯文本粘贴

## 当前改动

2026-10-07：实现中，需 CI 编译及协调者真机验收。

当前改动均为 overlay 文件和文档，不修改上游跟踪文件，不新增增量补丁。由现有 CI overlay 同步步骤处理；如果后续涉及上游补丁，必须先只读取得 runner 的当前文件，再用工具生成 diff，并模拟 CI 应用过程，不能手写补丁。

内核新增 `systemPasteAllowed` 菜单能力；判断依据是 Blink 的可编辑状态及 `kCanPaste`，不是内核剪贴板是否拥有内容。Chromium 154 的 `Editor::CanPaste()` 返回 `CanEdit()`，因此系统剪贴板没有读权限不会压掉这个能力。

参考手机外壳用 `hasDataType(MIMETYPE_TEXT_PLAIN)` 查询元数据，只有用户点击系统 `PasteButton` 成功后才读取正文。读取失败、为空或不是纯文本时提示用户重新操作，不回退到旧数据。菜单没有内核 `paste` 时补充系统粘贴入口；有内核 `paste` 时保留既有路径。

菜单粘贴复用 `insertText`，增加必填的原菜单 `requestId`。内核拒绝过期菜单、已切换/隐藏的标签页、已导航的文档及不匹配的焦点框架。有效请求通过原有剪贴板写入及 `WebContents::Paste()` 路径执行，以保留 paste/input 事件和撤销行为。这会将所粘贴的纯文本写回系统剪贴板，不保留原数据的其他格式。

手机地址栏的新增入口同样打开安全粘贴对话框；按打开对话框时的选区替换或插入，不自动提交搜索/导航，编辑已取消或上下文变化时丢弃结果。

## 不在本轮范围

- 不申请 `READ_PASTEBOARD`，不让内核直接读取外部剪贴板。
- 不改变 `OhosClipboard::GetAvailableMimeTypes()`；不能给网页宣称可读取实际上没有授权读取的内容。
- 不新增 `navigator.clipboard.readText()` 的外部读取路径。
- 不新增网页 Ctrl+V 的安全控件弹窗；浏览器自身复制后的原有快捷键路径保留。
- 不接桌面原生 Views 菜单；桌面采用自定义外壳菜单时可按新接口接入。
- 纯图片、富文本保真及多记录拼接暂不支持。

## 元数据查询依据

公开 API 中，NDK 的 `OH_Pasteboard_HasData`、`OH_Pasteboard_HasType`（API 13 起）以及 ArkTS 的 `SystemPasteboard.hasDataType`（API 11 起）未要求 `READ_PASTEBOARD`。公开服务实现也将查询与正文读取的授权路径分开；查询仍可能受数据有效性、分享范围、锁屏及平台差异影响。

- [NDK 头文件](https://github.com/openharmony/distributeddatamgr_pasteboard/blob/master/interfaces/ndk/include/oh_pasteboard.h)
- [服务实现](https://github.com/openharmony/distributeddatamgr_pasteboard/blob/master/services/core/src/pasteboard_service.cpp)
- [ArkTS 声明](https://github.com/openharmony/interface_sdk-js/blob/master/api/@ohos.pasteboard.d.ts)
- [PasteButton 授权示例](https://github.com/openharmony/docs/blob/master/en/application-dev/security/AccessToken/pastebutton.md)

本实现选择在外壳查询元数据；不因一个已复制的类型对网站暴露额外可读内容。公开 OpenHarmony 实现不能代替目标 HarmonyOS 设备的实测。

## 协调者真机验收

由协调者操作真机；开发 agent 不同时控制设备。先确认前台没有其他人正在使用的应用。

用 `docs/test-pages/system-paste.html` 验证：

1. 备忘录复制中文、英文、换行和 emoji，回到网页长按输入框：即使内核没有自己的剪贴板内容，也出现“粘贴”。
2. 普通菜单按钮不直接读取正文；必须再次亲手点击系统粘贴控件。成功后仅粘贴一次，支持选区替换和撤销。
3. 覆盖 textarea、contenteditable 及 iframe 输入框；只读/禁用目标不提供系统粘贴能力。
4. 菜单或安全对话框取消后不插入；读取过程中切换标签、导航、关闭页面、新开菜单，旧结果不得粘贴到新页面。
5. 从其他应用复制新文字后重复粘贴，不能回退到以前复制的文字；清空剪贴板或改为纯图片时，不误粘贴旧内容。
6. 地址栏全选替换、中间插入、部分选区替换均正确；取消编辑或切换上下文后丢弃旧结果，不自动访问粘贴的网址。
7. 浏览器内复制及原有 `paste` 仍可用；外部文本的网页 Ctrl+V 和 `readText()` 不计为本轮通过项。
8. 不申请受限权限；日志中不得出现剪贴板正文、网址或携带这些内容的异常对象。

原生内核在 CI 编译。当前 CI 设置 `SKIP_HAP=1`，引擎 HAR 构建不能证明参考 entry 外壳编译通过；外壳需在实际集成工程中额外构建，再真机验证。
