# OHOS 原生扩展手动更新

实现入口：`chrome://extensions` 开启开发者模式后的“更新”，以及外壳扩展管理页。
两者都通过 Chromium `ExtensionUpdater::CheckNow`，按普通 Profile 共用一个任务。
商店扩展使用 UpdateService，自定义更新源使用 ExtensionDownloader；不另写下载器。

## 外壳协议 v1

与本地交接文档 `LUMIE-EXTENSION-UPDATES-HANDOFF.md` 对齐：

- `getExtensionUpdateCapabilities { requestId }` → `extensionUpdateCapabilities`
  含 `protocolVersion:1`、`supported`、运行期随机 `contextId`。
  本地查询，不联网；无痕、访客、服务未就绪返回不支持。
- `checkExtensionUpdates { requestId, contextId }` → `extensionUpdateProgress`。
  每次发送完整 `items` 快照，`sequence` 按订阅严格递增；最后 `finished:true`。
  错误也用此事件收尾，不以 `extensionCommandFailed` 代替。
- 每项包含真实扩展 `id`、当前已安装 `version` 和 `status`。
  支持 `checking`、`downloading`、`installing`、`updated`、`upToDate`、
  `manual`、`permissionsRequired`、`pendingReload`、`failed`。
- `upToDate` 仅来自原生服务明确的无更新结果；`updated` 必须在 registry 中确认
  已安装版本高于任务启动时版本。下载完成或普通完成回调都不是成功证明。
- 无更新源的非商店 CRX、解包扩展显示 `manual`；受管理策略阻止的项显示 `failed`。
  网络、验签和安装失败显示单项 `failed`，不传原始 URL、路径或异常。
  任务级错误包含 `invalidContext`、`unavailable`、`timeout`。

## 行为边界

- 保留 `disable-background-networking`、`disable-component-update` 和 Google 同步禁用。
  仅 OHOS 的前台扩展更新请求可以通过两条下载通道的后台联网门禁。
- 用户停用状态、签名校验、版本比较、权限增量及安装均沿用 Chromium。
  不自动授予新权限或调用启用。手机外壳首版只展示权限状态。
- 设置页面关闭不取消任务；新窗口/重试订阅同一 Profile 的完整快照。
  浏览器窗口关闭移除其订阅，Profile 销毁使旧任务回包失效。
- 170 秒仍未完成，向订阅者返回 `timeout`，未确认的项目为 `failed`；
  原生任务继续，重试仍不会创建重复下载。原生回调返回前保持任务占用。
- 卸载为该任务的永久失败状态；OHOS 原生安装器在最终注册之前核对原安装实例，
  避免解包期间卸载/重装后被迟到更新覆盖。原生服务原有的“已卸载则拒绝更新”仍保留。
- 本次不增加自动更新调度或后台常驻能力。

## 验证

本机 `scripts/test-extension-updates.sh` 验证结果判定，覆盖失败不冒充已最新、
下载不冒充安装、待重载、权限增加、卸载后的迟到结果等。
`scripts/check-upstream-adapter.sh` 检查固定 Chromium 154 的补丁兼容性。
真实 CRX 更新、网络、权限及多窗口生命周期必须用构建产物验收，不能由静态测试代替。

真机测试（本轮不操作手机）：

1. 原生 `chrome://extensions` → 开发者模式 → 更新；外壳入口查询到 v1 才启用。
2. 安装可信源旧版（保持同一 ID/签名）后更新，确认版本刷新、设置保留，重启仍为新版。
3. 已最新版、无更新源、断网、坏签名分别准确显示结果；失败后旧版仍可用。
4. 停用扩展再更新，确认仍停用；权限增加不能自动启用；待重载不能显示已生效。
5. 同时点击原生页和外壳更新、跨窗口/超时重试，确认同一 Profile 不重复下载。
6. 检查中关闭窗口、卸载扩展、进入无痕，确认不串回包、不重新安装已卸载扩展。
