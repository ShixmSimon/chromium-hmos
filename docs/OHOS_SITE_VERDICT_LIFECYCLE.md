# 站点检测结果与同文档导航

2026-10-13。针对外壳 `2026-10-13-site-verdict-unchecked.md` 报告：手机 YouTube
显示华为服务最近可用，但当前站点仍为 unchecked、无检测时间、灰色图标。

## 结论边界

源码确认原实现存在两个缺口：`DidStartNavigation` 不区分同文档导航就重置快照，而
同文档导航不经过网络 throttle；`OnPolicy` 重置已提交页面结果，却没有安排补查。
两条路径都可能让 unchecked 持续到下一次网络导航。现场没有完整导航跟踪，因此
不能断言 YouTube 当时一定由其中某条路径触发。SDK 的成功日志不含网址，也不等于
当前站点的 no_match；外壳保持灰色的语义是正确的。

## 新行为

- 普通网络导航仍由原有 throttle 检测并在响应提交前阻断危险结果。
- hash、pushState、replaceState、同文档历史前进后退：开始时不清结果；提交时更新
  navigationId。完整 GURL 和策略版本一致时保留已完成结果和时间。路径、query 或
  fragment 变化均补查，保留本地 URL 精确规则的语义。
- 页面加载后授权、切换来源、启用防护或本站检测：重置旧身份下结果，对当前已提交
  且可检测的页面补查，不要求刷新。全局/本站关闭则 skipped，不发新查询。撤销云授权
  仍遵循 Service：仅华为来源无法检测时为 unknown，不会推断安全；同时来源可用本地结果。
- 补查为 checking → 实际结果，更新 checkedAt 和订阅命中信息。预算仍为 1500ms，使用
  同一个 Service，因此授权、云端 origin/path 脱敏、缓存、超时及并发限制不变。
- 新网络导航、同文档提交、策略变化、标签销毁都会取消对应补查并使旧回调失效。
  状态还校验请求代次、策略版本、完整 URL。恢复到相同地址也不能接收已取消请求。
- 旧文档在新网络导航等待期间发生 History API 导航时，只更新旧文档的已提交快照，
  不抢占待决导航；后者取消后再恢复并按需补查。
- 补查命中危险网址时，使用 Chromium `LoadPostCommitErrorPage` 加载原生安全警告并
  关联 `SecurityInterstitialTabHelper`。此路径不发送网页请求、不自动重放 POST，
  也不提供“仍要访问”。已是错误页或安全警告页时不重复补查/创建警告。

`getSiteProtectionState` 仍仅读状态；没有新增桥接命令或变更字段。外壳继续按原生事件
失效面板、重新读取，并检查 contextId/tabId/navigationId/revision 和 requestId；
不能依据服务 available 或其他 URL 的结果给当前页面标记安全。

## 验证

把生产快照状态转换提取到 `site_verdict_state.h`，CI Linux 原生测试覆盖：同址结果保留、
路径/query/hash 变化、旧导航身份失效、请求取消后同址迟到回包、策略版本变化、关闭与
重新启用、无授权 unknown、待决网络导航与 History API 交错、取消恢复、一次继续边界、
订阅风险元数据、重定向的真实危险目标，以及同址刷新不得复用快照。

这是生产状态逻辑测试；WebContents 回调顺序、原生警告页交互、真实华为 SDK 和外壳
读事件时序仍需设备集成回归。原生编译与 HAR 打包由 CI 完成，本轮没有操作真机或修改 Lumi。

## 设备回归

1. 新 HAR 完整重启，华为来源与已授权状态下打开 YouTube，等到真实结果和时间出现；
   记录切换视频、页面内跳转、后退后的 checking/最终结果，不能长期停在 unchecked。
2. 在可控的无害 HTTP(S) 测试页用 hash、pushState、replaceState 分别修改路径、query、
   fragment，并用相同 URL 的 replaceState 验证时间保持。需要完整 URL 风险命中时，
   仅以自有无害域名的指定测试 URL 建本地规则，验证危险 URL 转到原生警告。
3. 先关闭防护并打开页面，再启用/授权；验证无需刷新就补查。切换华为/本地/同时，
   以及关闭、撤销授权、本站开关，核对结果与时间归属于最新策略。
4. 检测期间立即换页/换标签，快速前进后退、关闭标签；慢 SDK 回包不得改写新页面。
   在新导航等待时修改旧页面历史，取消新导航后确认恢复结果或完成补查。
5. SDK 不可用/超时应为 unknown（或同时来源的实际本地结果），不应为了图标变色变成
   no_match；内部页/错误页不应触发云查询循环。

可开启 `navigation_throttle` 的 VLOG(1) 收集 `OHOS site verdict` 日志。字段为 reason、
navigation、policy、verdict、checkedAt、pendingNavigation；reason 区分
`navigation_start/redirect/navigation_commit/navigation_abort/same_document_commit/`
`policy_change/committed_check_start/committed_check_result/navigation_result`。
日志不包含 URL、origin、query、fragment 或网页内容。
