# 防钓鱼规则更新与内核交接

本文描述产品签名更新路径。2026-10-12 新增的推荐源和自定义 HTTPS 订阅使用独立 [规则来源接口](OHOS_PROTECTION_RULE_SOURCES.md)，无需产品签名公钥；这些源不会被标成官方签名规则，也不会覆盖本文的防回滚状态。

二期内核已实现可配置的签名规则更新链路。正式分发地址和公钥尚未配置，按用户要求默认关闭在线更新；不生成或提交生产私钥，不把测试签名钥匙装入应用。

## 外壳设置与接口

`getAboutInfo.urlProtectionApiVersion=2`，站点面板仍是 `siteProtectionApiVersion=1`。用户设置必须提供四项：

| 设置 | 原子配置 |
| --- | --- |
| 华为检测 | enabled=true, provider=huawei |
| 本地检测 | enabled=true, provider=local |
| 同时检测 | enabled=true, provider=both |
| 关闭保护 | enabled=false，保留所选 provider |

沿用 `setUrlProtectionConfig {requestId,contextId,expectedRevision,enabled,provider,cloudConsent}`。华为授权仍由外壳管理，不因选择云来源自动视为同意。配置失败不部分应用；切换取消旧检查，已提交页面不自动刷新。设置 UI 由外壳实现，内核提供已确认状态。

新增 `updateUrlProtectionRules {requestId,contextId,expectedRevision}`，返回 `urlProtectionRulesUpdateResult`，包含 requestId、ok、reason 和完整保护状态。ok 只表示已启动或复用正在执行的更新，不表示下载/验签成功；后续通过 `urlProtectionStateChanged` 观察结果。

手动更新只允许发起命令的 Profile 已启用 local/both，contextId/revision 需要匹配。仅华为或关闭模式返回 disabled，不因其他 Profile 使用本地规则而获得手动触发权限。请求不接受 URL、公钥、文件路径或未经核实的规则内容。

新增状态：

- rulesUpdateSupported：应用包中存在有效更新配置。为 false 时不得显示可用的在线更新入口。
- rulesUpdateStatus：initializing/not_configured/invalid_config/disabled/idle/updating/backoff。
- rulesUpdateReason：固定原因码，下载、签名/格式、过旧来源、回滚、读取、写入等失败不包含 URL 或服务器原始错误文本。
- rulesLastCheck、rulesLastSuccess、rulesNextCheck：UTC Unix 毫秒，未知 -1；时间是当前进程观察值。
- rulesSequence：已接受的递增序号；rulesSource：none/package/download。

原有 localStatus、rulesVersion、rulesUpdatedAt、rulesExpiresAt 继续描述实际索引。更新失败不把有效旧索引改成不可用；过期索引停止硬拦截。SDK 不可用不自动改用未选择的来源。

## 可信配置和打包

应用包可携带 `ohos_phishing_updates.json`，示意结构如下，示例公钥是占位说明，不可直接打包：

```json
{
  "format": 1,
  "manifestUrl": "https://distribution.example.org/phishing/manifest.json",
  "minimumSequence": 1,
  "keys": { "release-1": "<32-byte Ed25519 public key, base64>" }
}
```

配置只从应用运行时资源读取，最大 16 KiB，最多 4 把公钥，keyId 为 1–32 位字母/数字/下划线/连字符。HTTPS 地址必须是公网形状且不含用户凭据、query 或 fragment。域名形状不保证能识别所有 split-DNS，正式分发域名由产品控制。

构建设置 `PHISHING_UPDATE_CONFIG_FILE`，脚本校验后打入资源及 runtime.version。HAR 解包配置；新版本不包含该资源时删除旧配置，不能沿用上个应用包的更新端点。可选基础快照仍由 `PHISHING_RULES_FILE` 提供。

公钥轮换通过应用升级：先同时信任新旧钥匙，服务端改用新 keyId，后续升级移除旧钥匙并提高 minimumSequence。规则清单不能自行扩展信任公钥。

## 签名协议和工具

规则 JSON 沿用格式 1：来源 Phishing.Database、固定 sourceCommit/sourceSha256、MIT 许可、源时间/到期时间和精确 URL 列表。最长有效期 7 天、最大 16 MiB/20 万条，不能通过重新签名延长旧数据有效期。

发布清单是 `{format:1,keyId,signed,signature}`。signed 是元数据 JSON 原始 UTF-8 字节的 base64；signature 为这些原始字节的 Ed25519 签名，不对解码后重新序列化的数据验签。元数据包含 format、sequence、rulesUrl、size、sha256、count、version、generatedAt、expiresAt。version 为固定来源提交，sequence 是独立递增整数，最大 2147483647。

客户端先验签再解释元数据，拒绝未知钥匙、无效签名、过大数据、过期/未来时间、超长有效期和旧序号。rulesUrl 必须与可信清单地址同 origin。规则下载必须匹配声明大小、SHA-256、条目数、版本与时间，然后由生产 GURL 解析并建立精确哈希索引。不同 query/fragment、scheme、端口和子域不能扩大合并为域名封禁。

离线签名工具依赖 `cryptography>=45,<47`，运行示例：

```sh
python3 scripts/phishing-update-tools.py \
  --snapshot rules.json --sequence 42 \
  --rules-url https://distribution.example.org/phishing/v42.json \
  --key-id release-1 --private-key /secure/location/signing.pem \
  --config product-phishing-updates.json --manifest-output manifest.json
```

工具先核对来源有效期，再签名并用配置中的公钥验证；不会输出私钥或规则 URL。生产私钥只放发布系统的受保护存储，不能提交仓库。先发布不可变规则文件，最后原子发布清单；撤销误报也发布更高 sequence 的完整版本。

## 下载、存储和取消

同一普通 Profile 与所属无痕 Profile 共享只读公共索引和更新调度；不共享用户 URL、华为查询缓存、同意记录或站点例外。至少一个使用方选择 enabled local/both 才调度网络更新。最后一个使用方关闭或销毁时停止定时器、取消正在下载的请求。

启动后加 10–30 秒抖动，正常更新间隔为 6 小时加 0–30 分钟抖动；失败从 5 分钟指数退避至约 6 小时。手动请求合并正在执行的任务，受退避及最短一分钟请求间隔约束，不能绕过限流。清单和数据各有 30 秒网络超时。

下载使用 Chromium 网络栈，关闭凭据和缓存，禁止登录提示、禁止重定向，不携带网页 Cookie、Referer 或历史 URL。响应体分别限制 16 KiB 与签名声明大小（最多 16 MiB）；临时数据解析、哈希与落盘在工作线程完成，不阻塞导航 UI。

缓存保存原始清单和规则字节，原子替换；每次启动重新验签和校验内容。已接受的最高序号与 SHA-256 保存在普通 Profile 中，防止降级下载覆盖新规则；缓存丢失或损坏可重新下载相同序号及摘要的有效包，同序号不同内容被拒绝。有效旧库在错误时继续使用，已过期规则不再命中。关闭保护后，已完成下载、正在本地验证的数据可以完成公共缓存写入，但不能再发起网络请求或改变关闭状态下的导航决策。

## 数据源核对与运行验收

2026-10-11 核对发现，旧 GitHub `Phishing.Database/phishing-links-ACTIVE.txt` 的文件提交时间是 2025-12-22，大小约 65.8 MB，不能直接当作当前 16 MiB/20 万条规则包。维护者 README 指向 `phish.co.za/latest/` 及 checksums 仓库；本次官方文件端点返回 HTTP 503，活跃链接摘要文件最后提交时间为 2026-10-02，已超过本方案 7 天有效窗口。仍在更新的 phishing 仓库是提交/纠错源，不能把其任意新提交时间套给旧活跃库。

因此本轮没有内置或发布过期/截断的真实库，也不把假测试样本当生产威胁数据。上线分发需要确认活跃源的新鲜度、规模与许可，提供正式公钥/端点；如需改变容量或采用分片/二进制格式，需同步调整构建和客户端约束。

CI 的本机签名工具测试与 Linux host 原生测试使用仅测试钥匙和 example.com 规则，覆盖合法包、错签、未知钥匙、回滚、未来/过期、越界、跨 origin、摘要/计数/长度篡改、精确匹配及缓存格式。生产钥匙和测试夹具不会加入 HAR 运行时资源。

验证记录（2026-10-11）：[原生/HAR CI 38149304314](https://github.com/miramira8295/chromium-hmos/actions/runs/38149304314) 全部通过，原生验证器输出 `Native rule validation: 0 failures`，并提供联调 HAR artifact。[Adapter CI 38149384352](https://github.com/miramira8295/chromium-hmos/actions/runs/38149384352) 通过，包括协调器 14 项、快照工具 5 项、签名工具 8 项及固定上游补丁兼容性检查。隔离副本另验证了旧补丁升级、首次/重复应用；没有手工修改 runner 源码。

真实华为命中、手机/大屏导航、重定向、POST、BFCache/预渲染、站点例外和多窗口/无痕的设备联调仍须使用外壳与设备完成。本仓库不操作设备；构建通过不能替代这些运行验收。
