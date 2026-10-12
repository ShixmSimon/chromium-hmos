# 可配置的本地威胁规则来源

日期：2026-10-12。内核/HAR 实现推荐源目录与用户 HTTPS 订阅，外壳负责设置页面。四种检测模式仍为华为 / 本地 / 同时 / 关闭。`urlProtectionApiVersion=2` 和 `siteProtectionApiVersion=1` 保持兼容，新增 `ruleSourcesApiVersion=1`。

## 两条信任路径

- 产品签名规则沿用 `updateUrlProtectionRules`：应用包配置公钥与端点，Ed25519 验签、序号/摘要防回滚。当前未配置生产端点；不会因添加订阅而改写此配置。
- 推荐源与自定义源都是用户明确选择的 HTTPS 订阅，`trust=user_https, signed=false`。推荐不代表经过产品签名，也不代表对每条规则做过人工确认。每源独立配置、缓存、状态和版本。普通 Profile 可以管理，无痕只读并复用公共索引；无痕不能添加、修改、删除或手动刷新。

推荐目录默认不安装、不启用、不下载任何源。用户选择后才创建来源：

| recommendationId | 名称 | 地址 | 格式 / 匹配 | 许可 |
| --- | --- | --- | --- | --- |
| hagezi-tif-mini | HaGeZi Threat Intelligence Feeds Mini | https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/tif.mini-onlydomains.txt | domains / host_suffix | GPL-3.0 |
| phishdestroy-content | PhishDestroy Active Content | https://raw.githubusercontent.com/phishdestroy/destroylist/main/dns/content_active.txt | domains / host_suffix | MIT |

接口返回 license、attribution、licenseUrl，外壳应展示来源归属及许可链接。自定义源许可为 unknown，不能自动标成 MIT。仓库和 HAR 不内置两份完整规则；CI 下载固定版本的完整数据及许可证作基准，不将其打包。未来如镜像、修改或随应用分发列表，须按对应许可保留声明、修改说明及必要源文件，不能沿用旧 Phishing.Database 的 MIT 声明。

## 桥协议

命令经 BrowserCommandChannel 发出，事件经 WebWindow.onRuntimeEvent 接收；HAR 导出 `ProtectionRuleSource`、`ProtectionRuleSourcesEvent`、`ProtectionRuleSourcesChangedEvent`。

| 命令 | 参数 | 返回事件 |
| --- | --- | --- |
| getProtectionRuleSources | requestId | protectionRuleSources |
| addProtectionRuleSource | requestId, contextId, expectedRevision, enabled，以及 recommendationId 或 name/url/format/ruleType | protectionRuleSourceResult |
| updateProtectionRuleSource | requestId, contextId, expectedRevision, sourceId，以及需要改变的 name/enabled/url/format/ruleType | protectionRuleSourceResult |
| removeProtectionRuleSource | requestId, contextId, expectedRevision, sourceId | protectionRuleSourceResult |
| refreshProtectionRuleSource | requestId, contextId, expectedRevision, sourceId | protectionRuleSourceResult |

这里的 `expectedRevision` 取最近读取的 **sourcesRevision**，不使用全局 policyRevision。contextId 仍取当前窗口 Profile 的原生身份。sourceId 由内核生成，不使用 URL 作身份。推荐源可更名、启停和删除，其 URL/格式/匹配方式固定；另加自定义源才能使用其他语义。相同 URL 和 ruleType 的重复来源被拒绝。

设置来源可以在保护关闭时完成；只有调用方启用 local/both 且该源 enabled=true 时才能手动刷新。自动下载由原始 Profile 及无痕的本地保护使用方共同计数，最后一个使用方关闭时停止。没有启用来源则不调度下载。

所有命令回包包含 requestId、ok、reason、contextId、readOnly、sourcesRevision、ready、localSourcesAvailable、sources、recommendations、maxSources/maxRecordsPerSource/maxTotalRecords。add 的结果中来源列表包含新 sourceId。`ok` 表示配置操作已应用或刷新任务已开始/合并；刷新真正成功须观察后续状态。一个 Profile 同时最多一个订阅更新任务，同源请求返回 already_updating/ok，另一源返回 busy。

错误码包括 invalid_context、read_only、initializing、stale_sources、managed、invalid_request、invalid_recommendation、source_limit、duplicate_source、unknown_source、disabled、busy、rate_limited。失败不部分应用；版本冲突后重新读取状态再提交。

`protectionRuleSourcesChanged` 只广播 contextId、policyRevision、sourcesRevision，不广播访问 URL、订阅地址或内容。窗口收到匹配自己 contextId 的通知后重新读取；旧请求回包按 requestId/contextId/sourcesRevision 丢弃。来源改变或成功安装新索引会清除旧查询缓存和一次继续许可，重判待决导航，不刷新已提交页面，不删除原有精确 origin 网站例外。

## 状态与有效期

每源包含 sourceId、name、url、format、ruleType、enabled、kind、trust、signed、recommendationId、license、attribution、licenseUrl、revision，另有：

- status：loading / disabled / updating / ready / expired / error / missing；available 单独表示当前启用且仍有有效索引。更新失败但有效旧库仍在时，status 可保持 ready，同时 error 给出失败原因。
- ruleCount：安装后去重的有效记录数；inputCount：安装版本的原始非注释记录数；indexBytes：实际索引向量容量字节数。失败不展示部分导入数量为成功。
- sourceUpdatedAt：首版对文本源不推断可信上游验证时间，返回 -1。列表头时间或仓库提交也不能代表逐条重新验证。
- fetchedAt：当前缓存成功获取时间；expiresAt=fetchedAt+7 天。未知为 -1。这里是用户订阅的缓存最长寿命，不是上游新鲜度背书；用户信任订阅服务器提供当前数据。
- lastCheck、nextUpdate：上次尝试及下一次自动更新计划，UTC Unix 毫秒，未知或未调度 -1。次数/退避状态属于当前进程，重启从有效缓存的抓取时间恢复计划。
- error：download_failed、read_failed、invalid_format、invalid_utf8、invalid_rule、empty_rules、byte_limit、record_limit、total_record_limit、write_failed、cache_invalid、cache_expired、cancelled；不含服务器原始错误或浏览网址。

自动刷新约每 8 小时加 0–30 分钟抖动；首次约 15 秒后开始。失败从 5 分钟指数退避至约 6 小时；手动请求也受每源至少 1 分钟及失败退避限制。重命名、启停不能绕过当前源的限流。下载失败保留仍有效旧库，过期停止匹配。完全相同内容的 HTTPS 成功获取可更新用户订阅的缓存寿命，不改变未知的 sourceUpdatedAt；签名源仍执行自己的来源时间与防回滚要求。

全局 localStatus 在任意启用来源或产品签名库有效时为 ready，其余源错误从来源列表单独展示；全部不可用时不能显示本地保护可用。原 rulesVersion/rulesUpdatedAt 等仍描述产品签名库，不能拿它们代表所有订阅。

## 匹配、网络与容量

UTF-8 文本（允许 BOM），一行一条，忽略空行与以 `#` 开头的整行注释。其余条目必须完全符合声明格式，遇到混合 URL/域名、hosts、Adblock/uBlock、通配符、正则等语法时整包失败。不会自动扩展为另一种格式，也不会静默截断。

- urls / url_exact：经 GURL 规范化，保留 scheme、有效端口、path、query、fragment 精确语义。
- domains / host_exact：仅当前域名。
- domains / host_suffix：域名及以点分隔的子域；不会把单个子域提升为可注册域。使用 Chromium IDNA/大小写规范化，去除一个尾点，拒绝 IP、私有地址、公共后缀及 PSL 的私有注册后缀（例如 github.io），不影响其他租户。

匹配仅用于 HTTP/HTTPS 主框架导航及重定向，仍服从精确 origin 网站例外。订阅列表未分类时命中为 other/通用风险，**明确的本地订阅命中会拦截**；华为 OTHERS 的既有非钓鱼语义不变。站点详情新增 matchedSourceIds 和 ruleType（多个类型同时命中时为 multiple），全局广播不携带访问 URL。

下载使用 Chromium 网络服务，HTTPS、无凭据/Cookie/Referer、无登录提示，拒绝全部重定向。除 URL 形状检查外，设置 `kURLLoadOptionBlockLocalRequest` 按实际连接地址阻止私网资源请求，不依赖 LNA 功能开关。订阅 URL 的 query 可由用户显式提供，禁止 userinfo 和 fragment；含令牌的订阅地址仅在管理读取回包中返回，不加入日志或全局广播。

每次响应解压后最多 16 MiB、每源最多 500,000 条输入记录（重复项也计入），最多 8 源；所有已装索引（包括停用但保留的缓存索引）总容量最多 1,000,000 个 SHA-256 哈希，即约 32 MB。产品签名规则的原独立上限不变。后台串行解析、排序去重和原子落盘，整体替换来源索引；替换期间额外允许至多 500,000 个候选哈希及有限原始文本缓冲。导航使用二分查找，suffix 按主机名层级查询，绝不逐条扫描。

缓存与签名库目录独立，启动时核对 endpoint/format/ruleType、大小、摘要和抓取有效期。删除或停用来源取消下载并递增内部代次，迟到解析/安装回包不能改变新状态；删除的缓存清理排在已经开始的串行写入之后。关闭全部本地保护不会再发起请求，已经开始的磁盘操作可以结束，但不会恢复保护开关。

## 验证边界

CI 原生测试运行生产解析与匹配代码，覆盖 IDNA、尾点、suffix 边界、私有注册后缀、URL query/fragment/port/scheme、损坏/混合格式、容量、有效期及缓存身份。固定完整 HaGeZi 236,628 条和 PhishDestroy 89,449 条通过脚本核对 SHA-256 后参与容量与时间/内存基准；真实域名只用于离线解析，不访问列表中的网站，也不打进 HAR。

构建与测试证据将在 CI 通过后补入。手机/大屏的网络取消、断网恢复、来源管理 UI、待决导航与无痕联调仍需外壳拿到 HAR 后在设备上验收；host 基准不能代替 ARM64 设备性能数据。
