# 防钓鱼功能：外壳开发计划书

日期：2026-10-11。状态：内核/HAR 第一阶段已在 feat/url-protection 实现；用户确认外壳已按计划开发，本轮只负责内核。只读核对 Lumi 的站点面板和 backend 需求后补齐了内核接口，未修改外壳文件。下方原计划保留为目标，已实现内容及差异见本节；当前协议以 SHELL_CONTRACT.md 为准。

## 当前可接入能力

HAR 已导出 UrlCheckCoordinator 及 executor/authorization/options 类型，处理 configure/check/cancel/closeContext/capabilities。内核提供 getUrlProtectionState、原子 setUrlProtectionConfig、urlProtectionStateChanged，contextId 按 Profile 隔离并由内核分配；不依赖新增窗口销毁钩子，导航单独取消，Profile 销毁发送 closeContext。

站点面板新增独立 siteProtectionApiVersion=1：getSiteProtectionState、setSiteProtectionEnabled、siteProtectionStateChanged。内核维护当前导航结果及原生身份，持久/无痕站点例外，拒绝旧页面或旧版本写入；不自动刷新页面。HAR 提供命令字段和事件类型，外壳将自己的 SiteProtectionBackend 对接此协议，并在窗口生命周期内管理回包和失效事件。详情及验收边界见 SHELL_CONTRACT.md 的“地址栏站点防护”。

外壳先读取 urlProtectionApiVersion 和状态，准备自己的同意记录，再注册全应用共用的 coordinator，为其注入 safetyDetect.checkUrlThreat executor 和实际同意校验。originPathVerified 在官方验证完成前保持 false。默认关闭、来源 local；当前云同意只存内存，外壳重启后重新确认，无痕单独授权。

当前规则仅支持包内快照，rulesUpdateSupported=false，不支持原计划 updateUrlProtectionRules。外壳通过内核状态展示 missing/ready/expired 等，不自行实现规则缓存。S0 官方资格/配额/裁剪兼容与无害测试、S3 设置 UI 与同意管理、S5 真机验收仍需外壳完成；S2 可直接复用 coordinator，详见 [实际接口](SHELL_CONTRACT.md)。

面向外壳 ArkTS 开发者及内核联调人员。总体设计见 [防钓鱼方案](OHOS_SAFE_BROWSING.md)，当前接口以 [SHELL_CONTRACT.md](SHELL_CONTRACT.md) 为准。外壳先检查能力版本；没有该字段的旧内核不支持新命令。

## 1. 交付目标

外壳接入 HarmonyOS 官方网址安全检测，为用户提供防钓鱼总开关及三种检测来源：

| 用户选择 | 行为 |
| --- | --- |
| 仅华为 | 官方云检测；不可用时提示状态，不自动启用开源检测 |
| 仅开源 | 内核本地匹配 Phishing.Database；不向华为发送网址 |
| 同时启用 | 两个已选择来源参与检测；任一明确命中风险即可拦截，一个失效时另一个继续工作 |
| 关闭保护 | 不发起检测；不影响浏览器原有 HTTPS 和证书校验 |

外壳负责官方接口、设置、隐私告知和状态展示。内核捕获完整导航链并决定放行/拦截。用户在网页内点击链接、脚本跳转、打开 PWA 等均应由同一内核路径覆盖。

## 2. 职责分工

| 工作 | 外壳 | 内核 / 规则分发侧 |
| --- | --- | --- |
| 华为服务资格与配置 | 核实申请、签名、身份、权限、额度、计费、地区和条款 | 提供集成需求 |
| `checkUrlThreat` 调用 | 实现 ArkTS adapter、错误归一化、限流与有界等待 | 决定何时调用、送检 URL 和剩余预算 |
| 用户设置 | 绘制界面、采集操作、展示内核确认后的实际设置 | 保存并原子应用选择，广播多窗口状态 |
| 云检测同意 | 告知、记录/撤销同意，校验版本，无痕会话独立处理 | 未获授权不调度；撤销时取消待决检查 |
| 主框架导航与重定向 | 无须监听地址栏来补做检测 | 统一捕获、取消、等待及决策 |
| 开源本地检测 | 展示来源、更新状态及更新操作入口 | 匹配器、规则下载/验签/更新、过期和误报处理 |
| 风险页和“仍要访问” | 保持导航栏、返回操作与可访问性兼容 | 原生风险页、一次继续例外及生命周期 |
| 后续广告屏蔽 | 预留独立设置区域 | 独立过滤器、规则和白名单 |

外壳不维护第二套 URL 信誉缓存，不自行合并多个来源的风险，不用弹窗代替内核安全页。开源库推荐随包内置基础快照并后台更新，打包位置由内核/构建侧确定；外壳不下载整个 GitHub 仓库。

## 3. 已确认的接口与现有基础

### HarmonyOS SDK

本机 SDK 已确认：

```ts
import { safetyDetect } from '@kit.DeviceSecurityKit';

// 接口形态示意，不是完整可发布实现。
const result = await safetyDetect.checkUrlThreat({ urls: [checkedUrl] });
```

- 起始版本：HarmonyOS 5.0.0 / API 12。引擎当前最低兼容 API 20；此接口本身不要求抬高最低版本。
- Syscap：`SystemCapability.Security.SafetyDetect`。
- 每次 1–10 个 URL，每项最多 4096 **UTF-8 字节**，不是 4096 个字符。首版桥接每次只传一个 URL，不跨窗口或无痕合批。
- 返回 `results: { url, threat }[]`，必须验证数量、对应 URL 及 threat；空结果或错配不是安全结果。
- `NORMAL=0`、`MALWARE=1`、`PHISHING=2`、`OTHERS=3`。
- SDK 注释说明有云协同和网络操作，并要求避免 UI 线程调用阻塞渲染；原型验证 Worker/TaskPool 的实际支持，不能仅因返回 Promise 就认定无阻塞。

证据路径：DevEco SDK 中 `hms/ets/api/@hms.security.safetyDetect.d.ts`、`hms/ets/kits/@kit.DeviceSecurityKit.d.ts` 及 `hms/ets/api/device-define/api-version/DeviceSecurityKit.json`。不要把 SDK 文件复制进公开源码。

尚未核实：正式应用服务资格、AGC/签名/权限配置、价格与限额、官方无害测试网址、服务地域、URL 裁剪支持及数据处理期限。SDK 声明存在不能代替正式签名真机验证，不能照搬 HMS Android 的 URLCheck 接入步骤。

### 现有系统服务桥

已有能力：

- `overlay/chromium-ui/engine/src/main/ets/utils/SystemServices.ets`：`registerShellSystemService(service, factory)`。
- `overlay/chromium-ui/engine/src/main/ets/utils/SystemServiceBridge.ets`：`SystemServiceHandler.handle(method, args): Promise<Object | undefined>`。
- `overlay/components/ohos_system_service/`：浏览器进程发送请求并接收结果。

外壳在第一个 WebWindow 创建前注册 `urlcheck`。factory 会为不同窗口的桥创建 handler，不能把第一个窗口的控制器或 UIAbility 当作全局唯一实例；晚注册也不能假定现有窗口会自动补注册。

当前桥会把 handler 抛出的异常消息写入 hilog 并返回内核。华为 adapter 必须捕获 SDK 异常并转成固定原因码，不把可能包含 URL 的 SDK message、完整 response 或 request 交给通用异常日志。

原通用 handler 接口没有销毁钩子和 Profile/无痕身份参数。第一阶段已在 urlcheck 协议中补充按 Profile 的 contextId/configure/closeContext，导航单独 cancel；外壳可以复用 HAR coordinator，真实设备隔离与销毁仍需联调。

## 4. 设置与状态设计

建议页面入口：设置 → 隐私与安全 → 防钓鱼保护。

页面内容：保护总开关、检测来源三选一、各来源实际状态、本地规则版本/更新时间、云查询说明、无痕云检测授权入口。

| 选项文案 | 说明文案建议 |
| --- | --- |
| 华为安全检测 | 联网查询网址风险；送检网址信息会发送给华为 |
| 开源本地检测 | 在设备上匹配钓鱼规则，定期更新，不上传浏览网址 |
| 同时启用 | 使用华为安全检测和开源本地规则 |

“启用了来源”与“来源实际可用”分开显示。例如：

- `华为检测：可用 / 尚未验证 / 待同意 / 当前不可用`。
- `本地规则：可用 / 正在准备 / 更新失败，现有规则仍有效 / 已过期`。
- 仅华为模式失败时不能显示“已切换本地保护”；同时模式华为失败、本地有效时可显示“华为检测暂不可用，本地检测仍有效”。

不显示“此网址绝对安全”。SDK 支持、注册成功和某次检测正常都不能独立证明未来服务一直可用；状态附带最近一次检查时间/原因，不能永久缓存为在线。

### 设置存储与迁移

- 总开关兼容既有 `safeBrowsing=off|standard|enhanced`。UI 首期只展示开/关；已有 enhanced 不代表更强保护，也不代表同意上传更多数据。
- 来源通过 `setUrlProtectionConfig` 的 `provider=huawei|local|both` 提交，不使用单独的 `urlProtectionProvider` 偏好键。
- 启用状态和来源必须作为一次原子更新提交并等待内核确认；不能先打开旧模式再切新模式，造成短暂调用了用户未选中的来源。
- 内核保存运行策略；外壳保存适用的同意记录并向内核同步授权状态。外壳不得把未获内核确认的 UI 选择当作已生效设置。
- 第一阶段默认关闭、来源 local；旧 standard/enhanced 不自动开启新保护，也不授予华为云查询权限。
- 多窗口使用同一 Profile 的设置快照；无痕授权状态与普通 Profile 分离，不持久化无痕 URL 或会话授权。

### 隐私授权流程

选择仅华为/同时模式时，在发出第一条云请求前说明数据接收方、发送字段、用途和关闭方式；明确路径也可能含个人信息。取消同意保持原设置，不擅自切换来源。

首版提议仅送 scheme/host/port/path，去掉 userinfo、query 和片段，并筛除敏感路径。该形式是否适合官方检测需 P0 核实；外壳不能补回完整 URL、读取地址栏替换送检 URL，也不能静默改变上传范围。官方不支持时返回不可用原因，由内核按当前已选来源处理。

无痕查询单独告知，授权仅当前无痕会话有效。撤销授权先在外壳阻止新 SDK 调用，再同步内核取消待决检查；已发出请求不能撤回，返回结果不得改变已撤销策略下的页面。

## 5. 内核 → 外壳协议草案

服务名 `urlcheck`，协议版本 `1`。保留既有桥封装：

```json
{"id":42,"service":"urlcheck","method":"check","args":{}}
```

桥负责回包 `id` 和唯一完成，handler 只返回 result，不自行调用 native completion。业务 `requestId` 独立于桥 `id`，在浏览器本次进程内唯一；外壳不从数组序号或标签顺序推断请求身份。

### capabilities

不发送网址、不做后台测试查询。返回当前声明支持、配置及所支持的查询模式；不等同服务在线探测。

```json
{
  "protocolVersion": 1,
  "supported": true,
  "configuration": "unverified",
  "queryModes": ["origin_path"],
  "reason": "not_verified"
}
```

`configuration` 为 `ready|unverified|unavailable`。示例值仅示意协议，origin_path 要在官方支持与原型验证后才可实际宣告。权限/同意按具体上下文处理，不用全局 supported 代替授权。

### check

请求参数建议：

```json
{
  "protocolVersion": 1,
  "requestId": "nav-108-hop-1",
  "contextId": "ctx-7",
  "policyRevision": 3,
  "url": "https://example.com/login",
  "queryMode": "origin_path",
  "timeoutMs": 800
}
```

- `contextId` 是浏览器生成的不透明会话上下文，供双方关联 Profile、普通/无痕及授权；仅在应用内传递，不发给华为、不入网址日志。其注册/绑定接口由 HAR 配套提供，未知上下文一律不查询。
- `policyRevision` 关联已确认策略快照。外壳持有对应上下文的已确认模式和授权镜像；镜像未就绪、版本不匹配、关闭、仅本地或未同意时，不调用 SDK。
- `timeoutMs` 是本次剩余预算，含外壳排队时间。内核对整次导航累计计时，暂定上限 1500 ms，重定向不重置。外壳拒绝非正数并限制最大值，不自行续期。
- 核验 HTTP/HTTPS、UTF-8 大小、查询模式、裁剪结果及已知非公网地址；不访问目标网站来验证网址，也不下载页面内容。
- 一次调用 `checkUrlThreat({urls:[url]})`，只传 SDK 必需的 URL，不附带 Cookie、页面内容、表单、Profile 信息和内部身份。

正常结果示例：

```json
{
  "protocolVersion": 1,
  "requestId": "nav-108-hop-1",
  "contextId": "ctx-7",
  "policyRevision": 3,
  "verdict": "phishing",
  "reason": "matched",
  "checkedUrl": "https://example.com/login",
  "elapsedMs": 120
}
```

SDK 返回 URL 必须与实际送检值对应；首版不接受任意相似 URL 的结果，规范化差异在原型核实后由双方统一规则。内核再次验证 ID、上下文、策略版本及 URL。

| 华为结果 | verdict | reason |
| --- | --- | --- |
| NORMAL / 0 | no_match | no_match |
| MALWARE / 1 | malware | matched |
| PHISHING / 2 | phishing | matched |
| OTHERS / 3 | other | other_category |
| 空数组、缺字段、错配 URL、未知枚举 | unknown | invalid_response |

OTHERS 不改标为 phishing；是否提示由内核策略决定。预期失败通过 `verdict:unknown` 返回固定 reason，不能返回 no_match。

| 条件 / SDK 错误 | reason |
| --- | --- |
| 未同意 / 模式已禁用 / 策略已变化 | consent_required / disabled / stale_policy |
| 801 / Syscap 不支持 | unsupported |
| 201 | permission_denied |
| 401 / 参数校验失败 | invalid_request |
| 1010800002 | network_unavailable |
| 1010800003 | cloud_unavailable |
| 1010800005 / 1010800006 | concurrency_limited / rate_limited |
| 1010800007 / 本地 deadline 到期 | timeout |
| 1010800008 | cloud_quota_exceeded |
| 1010800001 / 未分类异常 | internal_error |
| 主动取消 / 上下文销毁 | cancelled / context_closed |
| 不支持协议 / 查询模式 | unsupported_protocol / unsupported_query_mode |

校验阶段失败无需回显未校验 URL，内核按请求身份关联 unknown。SDK 数字错误码可以作为可选 `sdkCode` 留在应用内，不透传 SDK message。协议无法处理的方法返回经脱敏的固定错误；现有桥 `ok:false` 由内核统一当作 unknown。

### cancel 与上下文销毁

已提供幂等 `cancel {protocolVersion,requestId,contextId,policyRevision}`。取消令原 check Promise 有界完成为 unknown/cancelled，清理 timer 和引用；准确回包字段见当前契约及 coordinator。

SDK 没有已确认的物理取消能力。外壳取消后忽略迟到结果，但不能立即把仍在执行的 SDK 调用当作已释放的真实并发槽；否则快速切换页面会突破云并发限额。持续悬挂时熔断新调用并返回 unknown，不无限排队或重复补发。

内核在 Profile 销毁时发送 `closeContext`，导航结束单独取消请求；普通窗口关闭不销毁共享 coordinator。应用服务结束调用 `dispose()`；进程结束由内核自己的 deadline/dispatcher 销毁收口，不能承诺进程已退出后仍会收到外壳回包。

## 6. 外壳 → 内核控制接口

第一阶段已实现版本化控制接口，准确字段见 SHELL_CONTRACT；在线规则更新仍属后续计划。

| 命令 / 事件 | 用途与必要信息 |
| --- | --- |
| `getAboutInfo.urlProtectionApiVersion` | 当前返回 1；缺失视为旧内核，不能展示功能已启用 |
| `getUrlProtectionState` → `urlProtectionState` | requestId、contextId、policyRevision、enabled、provider、授权状态、华为状态、本地规则状态/版本/更新时间及原因码 |
| `setUrlProtectionConfig` → `urlProtectionConfigResult` | 原子提交 requestId、contextId、expectedRevision、enabled、provider、授权状态；返回 ok/reason 和完整实际快照，版本冲突后重新读取 |
| `urlProtectionStateChanged` | 多窗口及服务/规则状态变化通知；只能携带状态，不能广播正在检测的 URL |
| `updateUrlProtectionRules`（尚未实现） | 当前 rulesUpdateSupported=false，不展示更新入口；后续仅允许已启用本地来源触发更新 |

contextId 由受信任 native/HAR 为窗口绑定，外壳不能任意冒用其他窗口或 Profile。上下文与策略镜像必须在第一条 check 前就绪；策略变化先停止外壳不再允许的调用，再提交内核，直到成功回包与镜像同步才允许新云请求。异步迟到回包按 revision 丢弃。

同意撤销失败不能回滚为重新同意；外壳持续禁止调用并显示待同步。普通设置变更失败恢复显示内核实际配置，授权门仍按实际同意状态限制调用。旧内核忽略新命令时不能让设置长期等待，用有界超时显示“当前内核暂不支持”。

## 7. 生命周期、限流与日志

- 各窗口 factory 返回同一个应用级 coordinator，共享真实 SDK 并发预算；内部按 contextId 隔离策略和请求，不跨 Profile 复用授权。
- 不跨普通/无痕合并查询，首版不做外壳 URL 缓存；内核维护按 Profile 隔离的结果缓存，首版尚未合并同时发起的相同 URL 请求。
- 首版无等待队列，并发满时立即返回 concurrency_limited；并发/QPS 数值在 S0 核实。完整频率预算与退避仍需根据官方配额补齐，导航请求不做超出预算的即时重试。
- 不因每次进入前台、打开设置或创建窗口自动向华为发送测试 URL。恢复前台只恢复能力/配置状态，实际检测由内核调度。
- 关闭标签、销毁窗口、切换来源、关闭保护、撤销同意、退出无痕要清理相关待决状态。全应用服务被销毁时不可持有旧 UIAbility/XComponent。
- 日志只记录来源、固定原因码、数值 SDK code、耗时、并发和聚合计数；禁记 URL、完整入参/响应、SDK 异常文本。不把结果直接塞入可持久化 UI 状态或崩溃附件。

## 8. 外壳建议任务拆分

文件名称为建议，按外壳实际仓库目录调整：

| 任务 | 建议产物 | 依赖 / 完成条件 |
| --- | --- | --- |
| S0 官方能力验证 | 接入记录、费用/额度/配置清单、官方测试样本及延迟记录 | 正式签名目标设备；至少钓鱼/恶意软件/普通测试 URL 各 20 次，记录 P50/P95/最大值；未获官方测试样本明确标记未验收 |
| S1 SDK adapter | `HuaweiUrlThreatAdapter.ets` | 枚举和错误完整映射，参数/返回校验，线程方式确认，无敏感日志 |
| S2 桥服务 | `UrlCheckService.ets`、应用启动注册 | capabilities/check/cancel、上下文绑定、有界等待、多窗口和销毁；与 HAR 配套联调 |
| S3 设置与授权 | `UrlProtectionSettings`、`UrlProtectionConsentStore` | 三选一+关闭，普通/无痕隔离，原子配置、版本冲突和旧内核兼容 |
| S4 状态呈现 | `UrlProtectionState` 展示层 | 展示实际来源状态、本地版本、更新失败/过期；不把未发现风险写成已验证安全 |
| S5 联调与交付 | 自动化结果、真机矩阵、问题清单、外壳版本/内核构建号 | 下方验收通过；测试 mock 与正式检测可区分，发布包无测试后门 |

S0 和协议评审后，S1 可用可注入 mock 独立开发；S2/S3 需要内核上下文、设置及取消接口。广告屏蔽本轮仅预留独立设置位置，不占用防钓鱼来源字段，不提前展示可用广告保护。

## 9. 联调验收清单

1. 仅开源：浏览、重定向、恢复/PWA 均无 Huawei SDK 调用；本地测试命中由内核拦截。
2. 仅华为：正常/钓鱼/恶意软件官方样本映射正确；本地库不参与判定；官方失败不暗中启用本地。
3. 同时：分别验证两来源独立命中及冲突；另一来源 NORMAL/unknown 不覆盖风险。本地先明确命中时允许直接阻断并省略云请求。
4. 关闭/切换/撤销：快速切换期间无未授权调用；旧模式回包不能影响新页面，等待能结束，实际 SDK 并发不因逻辑取消失控。
5. 隐私：取消同意无云请求；无痕授权/缓存与普通隔离；关闭无痕后清理会话，日志及崩溃附件不含网址。
6. 失败：权限、API 不支持、断网、限流、超时、空数组、错配 URL、未知枚举分别测试；unknown 不标为 no_match，前台没有无限加载。
7. 多窗口：第二窗口、销毁首窗口、PWA、后台/前台、进程重启都能正确注册/恢复；多个 Profile 不串配置。
8. 兼容：新外壳+旧内核、新内核+未实现 urlcheck 的外壳均显示真实状态、不挂起、不假称保护已开启。
9. 真机：手机单进程与平板/PC 多进程分别测试；官方链路使用服务方无害样本，不访问真实活跃钓鱼网址验收。
10. 本地更新：外壳展示内核报告的有效/过期/失败状态，手动更新不绕过模式开关；离线不误称华为云保护在线。

性能沿用总体方案拟定指标：云模式首屏额外耗时 P95 目标 ≤ 300 ms，安全检查累计等待上限暂定 1500 ms；这些是目标，不是已达到的数据。SDK 调用、等待和回包不阻塞 UI，原型记录页面滚动/输入时的响应。

## 10. 开工与发布边界

外壳拿到本文后可先开展 S0 接入调研，并接入第一阶段 HAR coordinator；正式接口以 SHELL_CONTRACT.md 为准。本轮已实现内核/HAR 配套，没有修改独立 Lumi 工程、调用官方检测、操作真机、向外壳人员发送消息或部署服务。

发布前需要：官方服务条件明确、协议写入 SHELL_CONTRACT、实际内核能力就绪、三种模式/无痕/多窗口验证、正式签名官方命中证据和隐私说明完成。SDK 不支持或服务申请未完成可独立提供已验收的本地模式，但 UI 必须清晰展示华为来源尚不可用。
