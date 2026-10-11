# 恶意网站与钓鱼检测方案

状态：2026-10-11，P1 内核与桥接已进入主干并通过 CI，P2 签名更新链路已实现，尚未配置生产分发或完成真机验收。本文保留完整目标方案，实际已实现接口以 SHELL_CONTRACT.md 为准，更新实现见 [二期交接](OHOS_PHISHING_RULE_UPDATES.md)。

外壳开发交接见 [外壳计划书](OHOS_SAFE_BROWSING_SHELL_PLAN.md)，包含模式设置、官方 adapter、桥协议、内核配套需求和联调验收。

## 当前开发交付（P1 + P2 内核）

- 按 Profile 隔离的配置/检测服务，用户可在设置选择“华为 / 本地 / 同时 / 关闭”，通过 enabled/provider 原子应用；会话云授权、精确本地匹配与来源状态。
- 主框架/每跳重定向挂接，已知风险请求前阻止，其余查询与网络并行、提交前汇总；累计预算 1500 ms。原生风险页、当前标签准确 URL 一次继续，POST 不自动重发。
- 开启保护时取消预渲染，对受观察标签清理 BFCache 并禁止当前文档进入 BFCache，实际激活/设备行为仍需回归。
- 有界系统服务调用及导航取消，HAR 导出 SDK 独立的 UrlCheckCoordinator；外壳仍需注入真实 SDK executor、同意检查并注册。
- 地址栏站点面板配套：siteProtectionApiVersion=1，提供当前原生导航身份/结果读取和站点例外写入。按 origin 精确匹配，普通 Profile 持久化、无痕会话隔离；写入验证活动标签/导航/策略版本，取消旧检查，已提交页面不自动刷新。完整接口见 SHELL_CONTRACT.md。
- 包内快照生成/校验/打包/解包及 Ed25519 签名在线更新已实现；当前没有内置真实 feed，缺失报告 missing，不宣称已有离线保护。未配置生产端点/公钥时在线更新关闭，rulesUpdateSupported=false。规则限 16 MiB/20 万条、最长有效期 7 天，工作线程解析为精确哈希索引；有效旧库在更新失败时保留，缓存启动重验签，序列与摘要防回滚。
- 默认关闭、来源 local；旧 standard/enhanced 不自动启用，也不授予云同意。普通 Profile 启用/来源持久化，无痕变更及云授权为会话状态。
- 用户明确本轮只负责内核。只读核对了 Lumi 已有站点面板和 SiteProtectionBackend 需求，没有修改外壳；生产适配器接入由外壳完成。未调用真实华为查询、未开通官方服务、未操作真机。真实 feed 验收与分发部署仍需生产配置，广告过滤属于后续独立阶段。
- SDK 协调器 14 项、快照工具 5 项、签名工具 8 项测试通过；补丁首次/重复/升级及全新基线检查通过。[二期原生/HAR CI](https://github.com/miramira8295/chromium-hmos/actions/runs/38149304314) 已通过 C++ 构建、HAR 打包及生产 C++ 规则验签测试，[Adapter CI](https://github.com/miramira8295/chromium-hmos/actions/runs/38149384352) 通过。这些测试覆盖协调器、构建工具与原生规则格式，不覆盖原生导航/站点例外运行时；这些仍须真机验收。

## 推荐方案

提供 **HarmonyOS 官方检测、开源本地检测两个可独立选择的来源**，用户可随时切换，也可同时启用；统一使用 Chromium 原生拦截页，不强制双重检测。后续广告屏蔽作为独立模块，共享规则分发基础设施，独立开关、白名单和决策。

- 官方检测：`@kit.DeviceSecurityKit` 的 `safetyDetect.checkUrlThreat`，通过现有系统服务桥接入。
- 本地检测：首选评估 Phishing.Database 的活跃 URL 数据，在本仓库开源实现精确匹配器，离线也能识别已收录地址。
- 决策：仅运行用户启用的来源；同时启用时任一路明确命中钓鱼/恶意软件就拦截，另一路“未发现风险”或超时不能覆盖命中。
- 广告过滤：后续优先评估 Brave `adblock-rust`，先网络过滤，再元素隐藏与高级规则。

开源方案指“开源匹配器 + 开放许可威胁数据”。首期不做截图/DOM 上传和机器学习检测，未收录的新钓鱼网站仍可能漏检。

### 用户设置与模式

设置提供“华为 / 本地 / 同时 / 关闭”四项。通过 setUrlProtectionConfig 原子提交 enabled/provider/cloudConsent，内部使用 ohos.url_protection 配置字典；关闭使用 enabled=false，不增加 off provider，不新增可分步写入的 urlProtectionProvider 偏好键。默认关闭、来源 local，旧 standard/enhanced 不自动启用且不授予云查询同意。

| 模式 | 执行与失败行为 |
| --- | --- |
| 仅华为 | 只调用官方检测及其有效缓存，不匹配本地库；官方不可用时显示该来源不可用并放行未知项，不自动启用开源 |
| 仅开源 | 只使用本地库及规则更新，不向华为发送网址；库未就绪/过期时提示状态，不自动启用云检测 |
| 同时启用 | 两个来源都参与；本地已命中可直接拦截，不为已确定的结果额外外发 URL；一个来源不可用时继续使用另一个已启用来源 |
| 关闭保护 | 不运行任一来源的检查，不用历史风险缓存阻止新导航 |

切换立即作用于待决及后续导航：更新策略版本，取消已禁用来源的等待、清除其内存查询缓存，忽略迟到结果，撤销旧模式下的一次继续例外；已提交页面不自动重新加载。新增云查询仍须满足隐私同意。规则库文件可以保留供再次启用，只有至少一个启用保护的 Profile 选择 local/both 才调度规则更新。

## 仓库现状

核对基线：`hmos-154-adapter` / `34534541`。

| 已核实事实 | 依据及影响 |
| --- | --- |
| `safe_browsing_mode = 0`、`enable_arkweb = false` | `config/args.plan_kirin_pc.gn`；不能只打开 Google Safe Browsing 开关，也不能把 ArkWeb 目录已有接口当作本产品已实现 |
| 已有 `safeBrowsing` 偏好，未提供实际网址保护 | `overlay/chrome/browser/ui/ohos/shell_prefs_ohos.cc`、`docs/SHELL_CONTRACT.md`；验收前不承诺保护已生效 |
| C++ → ArkTS 系统服务桥已存在 | `overlay/components/ohos_system_service/`；可以复用 |
| 外壳能注册服务并应用到多个窗口 | `overlay/chromium-ui/engine/src/main/ets/utils/SystemServices.ets` 的 `registerShellSystemService` |
| 引擎 HAR 最低兼容 API 20 | `docs/SHELL_CONTRACT.md`；官方 URL API 声明起始版本低于 API 20，不需因此提高最低版本 |
| ArkWeb 下有广告规则实现 | 仅作参考；当前禁用 ArkWeb，仍需另行验证 Chromium 154 的过滤挂接点及依赖 |

## 调研结论

### HarmonyOS 官方检测：已核实 SDK 声明，未真机验证

证据为本机 DevEco Studio 随附 SDK（不复制或提交 SDK）：

- `D:/Applications/DevEco Studio/sdk/default/hms/ets/kits/@kit.DeviceSecurityKit.d.ts`
- `D:/Applications/DevEco Studio/sdk/default/hms/ets/api/@hms.security.safetyDetect.d.ts`
- `D:/Applications/DevEco Studio/sdk/default/hms/ets/api/device-define/api-version/DeviceSecurityKit.json`

已核实：

1. Kit 导出 `safetyDetect`，接口为 `checkUrlThreat(req: UrlCheckRequest): Promise<UrlCheckResponse>`，自 HarmonyOS `5.0.0(12)` 提供。模块设备元数据列出手机/平板 API 12、2in1 API 13；声明支持仍需各设备实测。
2. 请求 `{ urls: string[] }`，一次 1–10 个 URL，每项最多 4096 字节；返回 `{ results: [{ url, threat }] }`。响应需校验 URL 与请求身份，不能只信任数组位置。
3. 枚举 `NORMAL=0`、`MALWARE=1`、`PHISHING=2`、`OTHERS=3`。OTHERS 表示其他疑似违规，不能等同钓鱼或不受欢迎软件。
4. Syscap 为 `SystemCapability.Security.SafetyDetect`；SDK 明确涉及云端协同和网络操作，注释要求避免在 UI 线程调用而阻塞渲染。原型核实 Worker/TaskPool 支持，不因 Promise 形式就认定调用无阻塞。
5. 声明了权限拒绝、不支持、网络不可达、云服务失败、并发超限、频率超限、超时、云流量超限等错误，但没有给出具体额度。

待核实，上线前必须解决：

- 商用浏览器是否需申请服务，AGC 项目/应用身份/签名/权限/凭据配置，计费、免费额度、QPS、并发阈值和地区支持。
- 是否支持裁剪 URL，以及裁剪对识别率的影响；官方数据处理与保留条款。
- 官方无害的钓鱼与恶意软件测试网址；不能假设 Google 测试页会被华为识别。
- 正式签名应用在目标设备上的实际可用性、延迟与 API 20 构建 SDK 的编译兼容性。

本轮访问华为开发者文档站仅得到动态页面外壳，未获取指南正文。因此不套用旧 HMS Android URLCheck 的配置要求，也不把 SDK 声明当作本应用已获云服务使用资格。

### 开源与其他来源

| 候选 | 核实程度与结论 | 选择 |
| --- | --- | --- |
| [Phishing.Database](https://github.com/Phishing-Database/Phishing.Database) | 已读取 README 与 [MIT LICENSE](https://github.com/Phishing-Database/Phishing.Database/blob/master/LICENSE)；提供活跃 URL/域名和历史数据；[源仓库](https://github.com/Phishing-Database/phishing) 有误报纠正流程 | 首选本地数据，固定来源提交、许可和快照，评估活跃数据的新鲜度与中文站点覆盖；优先精确 URL |
| OpenPhish Community | 本轮未成功获取 feed 说明；免费不等于可商用再分发 | 备选，核对镜像、商用和再分发条款后再决定 |
| PhishTank | 本轮未取得开发者说明；访问条件、频率与许可待核实 | 备选，首期不增加依赖 |
| [Google Safe Browsing](https://developers.google.com/safe-browsing) / Web Risk | 官方页面已核实 Safe Browsing API 仅限非商业用途，营收用途指向 Web Risk；大陆可达性、费用和本项目用途未核实 | 本轮不默认接入，不假设 Web Risk 能无改动替换 Chromium 后端 |
| 国内其他商业信誉接口 | 未核实浏览器授权、合同与费用 | 暂不引入，保留 provider 扩展能力 |

Phishing.Database 不保证实时或无误报。历史全集不能直接作为永久封禁库。正式分发保留署名并核查固定数据版本的来源条款，不把任意免费 feed 自动视作允许镜像的数据。

## 运行架构与决策

建议新增浏览器进程的 `OhosUrlProtectionService`（按 Profile 管理）和 `OhosUrlProtectionNavigationThrottle`，放入 `overlay/chrome/browser/ohos/url_protection/`；名称为设计建议。

流程：主框架导航 → URL 解析及范围检查 → 按所选模式分发到本地匹配或官方查询（或两者）→ 浏览器进程汇总已启用来源的有效结果 → 继续或原生风险拦截页。下表仅适用于已启用来源，禁用来源的旧缓存不参与决策。

| 结果 | 行为 |
| --- | --- |
| 任一有效来源命中 phishing / malware | 拦截，另一来源 NORMAL 或 unknown 不能覆盖 |
| 本地未命中 | 只表示未收录，不能宣称网址安全 |
| 官方 NORMAL | 本次检查未发现威胁，不覆盖本地风险 |
| 官方 OTHERS | 保留独立分类，首期不按钓鱼硬拦截；待核实官方语义再决定提示策略 |
| 无权限、不支持、超时、断网、限流、异常响应 | 该来源记 unknown；其他已启用来源没有已知风险则放行，不把失败标成安全，不自动启用其他来源 |

### 检查范围与时机

- 首期覆盖 HTTP/HTTPS 用户可见主框架导航：地址栏、链接、新窗口、会话恢复、PWA 与每一跳重定向。子框架、子资源及下载内容扫描不在首期保护承诺内。
- 跳过内部协议、file/data/blob、localhost、单标签内网主机、`.local`、私有/回环/链路本地等非公网 IP，包括 IPv6 与映射地址。若已知解析结果为非公网也不外发；域名名称无法识别全部 split-DNS 内网，原型需验证解析信息的可用时机，不能承诺完全过滤。
- 带用户凭据、疑似一次性令牌、无效或过长 URL 不做云查询；只有选中本地来源时才继续本地检查，否则记为未检查。
- `WillStartRequest`、`WillRedirectRequest` 先匹配已启用的本地规则及已启用来源的有效风险缓存，命中立即阻止请求或继续重定向。索引异步加载，禁止 UI 线程同步磁盘读取。
- 官方来源已启用且未发现已知风险时，官方查询与网页网络请求并行，在 `WillProcessResponse` 提交网页前等结果；仅开源模式不等待官方。首期保证危险页面提交/执行前的拦截，不保证首次未知网站的请求未发出；POST 可能已传出表单，不能宣传能阻止所有凭据外泄。
- 暂定安全检查对一次导航引入的累计等待上限 1500 ms；多跳不重置预算。预算到期放行未知项，已知风险仍拦截。数值按真机 P50/P95 调整。
- 每跳独立身份和 URL，中间跳命中不能被末跳 NORMAL 覆盖。已放行后的迟到结果不能覆盖新导航或突然切换拦截页，校验通过的有效风险结果可用于后续导航。
- 导航取消、标签关闭、Profile 销毁、关闭保护、外壳销毁均须处理过期回调，确保单次恢复/取消。限制并发和排队，合并同 Profile 的相同检查，超限短路与退避，具体数值依据官方限额及原型。
- 禁止预取/预渲染触发云查询；首期对无法在展示前检查的预渲染路径暂时关闭预渲染。BFCache 恢复需独立验证挂接点与有效判定，不能当作普通网络导航；无法保证时暂禁该恢复优化。

## URL 粒度、隐私与缓存

使用 Chromium GURL 解析，规则构建端和客户端采用同一版本规范。覆盖 IDNA、大小写、默认端口、路径、百分号编码及查询参数，避免重复解码、任意排序 query 或把不同路径合并成域名；HTTP/HTTPS 默认分开。

- 本地精确 URL 保留路径与 query 语义；若来源有片段型规则独立表示，不能去掉片段后扩大封禁到所有同页地址。官方查询去掉 userinfo 和片段。
- 官方查询须先明确告知“网址将发送给华为检测”并取得适用同意。本地库下载不带浏览历史，不部署接收用户 URL 的自有查询网关。
- 默认建议云查询不发送 query，仅发送筛选后的 scheme/host/port/path；路径本身也可能含个人信息，不能称为匿名。如果服务不接受该形式或路径疑似带令牌，则跳过云检测，仅在本地来源已启用时继续本地检查；上传更完整 URL 需另行明确告知，不能静默升级。
- 保存原始导航 URL、实际送检 URL 及各自规范键的对应关系。裁剪结果不当作完整 URL 的安全证明，必须承认识别 query/片段型钓鱼的能力损失。
- 缓存键含 provider、规范化检查 URL、查询模式和规则版本，严禁只按 hostname 缓存。暂定未发现风险 5 分钟、风险 30 分钟，受来源期限约束；unknown 不写成安全缓存。
- 查询缓存、用户例外仅存内存；普通与无痕隔离，不跨 Profile 合并请求。无痕可共享只读公共威胁库；无痕云查询单独明确告知，未同意时不调用官方，仅在本地来源已选中时执行本地检查，否则显示无可用检测来源。关闭无痕销毁会话状态。
- `safeBrowsing=off` 停止该 Profile 的检测与新云查询，取消等待并释放导航；已发送的第三方请求无法撤回，其结果不再影响页面。没有启用保护且选择本地来源的 Profile 时停止威胁库专用更新，广告更新由广告开关控制。
- 保留 standard；首期不展示没有差异的 enhanced，已有 enhanced 值按 standard 兼容且不追加上传字段，实施时写入契约。
- 日志仅记录计数、耗时、原因码、来源和规则版本，不记录用户 URL/路径/query/完整响应。公共威胁规则包允许落盘，用户查询记录不落盘。

## 官方桥接协议草案

沿用 `registerShellSystemService('urlcheck', factory)`。虽然接口起始 API 低于引擎最低版本，仍由外壳管理 SDK、产品身份、服务配置与隐私授权，C++ 负责导航及决策。桥位于 browser 进程，网页不可直接调用。

- `capabilities` 返回协议版本、支持状态、查询模式及不可用原因。canIUse 成功不能代替云服务可用性。
- `check` 请求：`{ protocolVersion: 1, requestId, url, queryMode, timeoutMs }`。url 在浏览器端裁剪，外壳再校验，内部 Profile 身份不发送华为。
- 正常结果：`{ protocolVersion: 1, requestId, verdict, reason, checkedUrl, elapsedMs }`；verdict 为 `no_match | phishing | malware | other | unknown`。失败可用现有桥 `ok:false` 封装，内核统一映射 unknown。
- 校验 schema、ID、URL、响应大小、枚举，错配/缺项/未知枚举不能判为安全。内核独立执行 deadline，不只依赖 ArkTS Promise。
- SDK 未声明取消能力，逻辑取消后忽略迟到响应；实施需审查并清理桥内 pending 状态，避免长时间积累。

第一阶段已补充 contextId/policyRevision、configure/check/cancel/closeContext、原子配置和 HAR coordinator。contextId 绑定 Profile，导航单独取消，Profile 销毁清理上下文；通用桥 handler 无需新增窗口销毁接口。外壳可复用实现，正式设备生命周期行为仍需验收。

上面保留协议设计说明；当前实现的准确字段和方法见 SHELL_CONTRACT.md，正式发布前仍需联调验收。

## 本地规则分发

首期精确 URL 索引；域名规则仅在来源明确指向恶意主机时加入，禁止从单个恶意链接扩大封禁到共享托管域名/公共后缀。

推荐：上游活跃数据 → 定时构建、去重及误报纠正 → 签名版本包 → 静态 HTTPS 分发 → 客户端后台更新。CI/分发托管位置和带宽预算在实施前确定，不新增用户网址查询服务器。

- 首版用排序的完整哈希/精确索引。若加 Bloom filter 只预筛，必须精确复核，概率碰撞不能导致封禁。
- 暂定每 6 小时检查更新，加抖动及指数退避，不超过源允许频率；先全量包，实测规模后再决定增量。验证大陆目标网络下载成功率，不依赖每次启动直连 GitHub。
- 清单含格式版本、递增版本号、生成/到期时间、来源固定版本、许可证、包大小、记录数、SHA-256 与数字签名。真实性依赖内置公钥验签，摘要不能替代签名；设计防回滚和密钥轮换。
- 临时下载，限制压缩/解压大小和记录数，验签/解析成功后原子切换；失败保留有效旧包，不接受任意旧版本降级。
- 源停更不能刷新包时间伪装新鲜；条目和包设有效期，期限依据样本确定。过期到不能信任的规则停止硬拦截，准确提示“规则待更新”。
- 提供误报撤销和紧急纠错版本，区分误报、来源移除与临时不可达；历史全集不重新混入活跃库。某来源纠错白名单只作用该来源，不能覆盖华为或其他独立风险。
- 更新器只访问指定 HTTPS 端点及同 origin 规则地址，禁止重定向，不继承网页 Cookie/Referer，不访问规则中列出的网址；规则输入需按可能损坏或恶意内容处理。

## 拦截页

复用 `components/security_interstitials` 通用框架，新增 OHOS 控制器与中文资源。实施审查 safe_browsing_mode=0 下 GN 依赖，避免拉入 Google 后端。

- 说明风险类型与来源，突出主机、正确展示 IDN 和长地址，不暴露敏感 query。
- 主操作“返回安全页面”：返回安全历史项，没有则打开新标签页，不能再次返回已拦截站点。
- 次级“详细信息 → 仍要访问”：只授予当前标签、准确 URL 的一次显式继续例外，不整站、不跨标签、不持久化；新重定向重新检查。POST 不自动重发。
- 分别显示用户选择与实际可用状态；仅华为模式云不可用就显示该来源不可用，同时模式才显示剩余本地保护。所有已选择来源不可用时显示保护不可用，不显示绿色安全认证。
- 误报反馈只在用户主动操作并确认发送内容后提交，不自动上传网址。

## 后续广告屏蔽

首选评估 [Brave adblock-rust](https://github.com/brave/adblock-rust)。已读取 README，项目标注 MPL-2.0，支持网络过滤、元素隐藏、资源替换、Hosts 和部分 uBlock 语法；不能据此承诺全部 uBO 功能。需验证 HarmonyOS Rust/GN/FFI 集成、体积、性能和许可证义务。

1. 网络过滤：选择 EasyList 与中文规则候选订阅，逐一核对固定版本许可和再分发权。提供站点暂停、计数与基础诊断，验证 script/image/XHR/fetch/iframe/媒体及例外规则。
2. 元素隐藏：CSS 隐藏与动态网页/多 frame 生命周期；仅网络过滤不能保证广告区域消失。
3. 高级兼容：再评估 scriptlet、资源替换、自定义订阅；只用随版本审核的能力，不让任意订阅下载可执行脚本或获得浏览器权限。

广告走高频请求过滤路径，不对每个子资源调用 ArkTS/云端；browser/network/renderer 挂接点待原型验证。Service Worker、缓存、WebSocket/WebTransport、重定向、多进程逐项验证，未覆盖能力明确列出。

共享规则下载、验签、更新状态和任务调度；独立规则命名空间、索引、优先级及设置。关闭广告/添加广告白名单不会关闭防钓鱼；恶意站点拦截优先于广告允许规则。本地安全匹配不依赖 Rust 广告引擎。

## 分阶段交付

| 阶段 | 交付 | 完成条件 |
| --- | --- | --- |
| P0 官方与数据原型 | 服务申请/配置、条款限额、无害测试样本、延迟和活跃数据质量报告 | 正式签名应用可用；钓鱼、恶意软件、普通测试 URL 各至少 20 次查询，记录 P50/P95/最大值；无官方样本则明确标记尚未验证，本地开发可独立推进 |
| P1 内核与外壳闭环 | Profile 服务、导航拦截、官方桥、本地匹配、受控测试包、原生拦截页、设置与隐私 | mock 的命中/正常/超时/取消/重定向/多标签/无痕通过；真机区分 mock 与官方结果，测试库不进入正式包 |
| P2 防钓鱼发布 | 签名规则分发、真实活跃库、误报纠正、限流降级、保护状态 | 官方命中、离线本地命中、更新失败及恢复、手机/大屏验收通过，才宣称保护已生效 |
| P3 广告网络过滤 | 引擎验证、订阅、站点设置、请求过滤 | 性能与站点兼容达标，广告白名单不影响防钓鱼 |
| P4 元素隐藏与兼容 | CSS 隐藏及后续高级规则 | 动态网页、多 frame、多进程回归通过 |

本轮已开发 P1 运行时代码与 P2 签名更新链路及构建工具；P2 的生产数据和设备验收条件尚未满足。未开通官方服务、打包真实规则库、部署在线分发或操作真机。用户授权先完成可配置的更新链路，正式端点和公钥缺省关闭。

## 验收标准与工程约束

拟定性能目标（不是测量结果）：本地热查找 P95 ≤ 5 ms；20 个常用国内站点各重复 5 次做开/关配对对比，本地模式首屏额外耗时 P95 ≤ 50 ms，云模式目标 ≤ 300 ms；检查累计等待硬上限 1500 ms；额外内存目标 ≤ 30 MiB，依据真实规则规模确认。云端目标不达标时优化缓存/并发/默认模式后复测，不直接算通过。

必须覆盖：

- 官方无害钓鱼/恶意软件样本和受控本地 fixture 分别证明两路工作，不访问真实活跃钓鱼站验收。
- 三种模式及总关闭分别验收；仅开源无官方调用，仅华为无本地匹配和规则更新（其他 Profile 正在使用本地库除外）；切换取消旧等待且旧缓存/迟到回调不参与决策；单来源失败不自动启用另一来源。
- 同域名不同路径/query、IDN/编码、子域/公共后缀、短链多跳、POST、前进后退、BFCache/预渲染、恢复与 PWA。
- 一方命中另一方 NORMAL/unknown，错配 URL/缺项/未知枚举，不支持/权限不足/超时/限流，取消/迟到回调、多 Profile/窗口/标签。
- 服务不可达而普通站点可达时正常浏览；完全断网仅验证本地匹配/缓存页，不要求远程网页仍可加载。
- 一次继续及新重定向检查，关闭保护无新查询，无痕无用户缓存落盘且与普通模式隔离。
- 错签/损坏/过大/旧版/过期包、源停更、更新失败保留有效版本、纠错撤销误报。
- 手机单进程和平板/PC 多进程；日志无用户 URL 内容泄漏；保护设置不改变证书错误和 HTTPS 校验。

工程沿用仓库约束：新源文件放 overlay；修改上游文件先只读取 runner 对应文件，再生成增量补丁，不手写；本地隔离目录验证首次/重复/升级应用，登记 scripts/ci-incremental-build.sh；GN 依赖加入已初始化的 deps；只在 CI 编译，真机由协调者验收。实施阶段更新 SHELL_CONTRACT.md、外壳交接文档和第三方许可清单。
