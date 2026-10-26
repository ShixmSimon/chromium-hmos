# HarmonyOS 安全 DNS 与 ECH

> 隐私处理：私人测试域名已替换为 `ech-test.example`。命令示例需换成自己的测试域名；原始证据仅保存在本地。

154（Chromium 154.0.8037.97）和 155（155.0.8059.91）采用相同的新增补丁、设置协议和验收方法。

## 实现范围

- `patches/ohos-secure-dns-ech.patch`：将 OHOS 加入 `AsyncDns` 默认平台；为 POSIX DNS 配置服务增加 OHOS 分支，使用既有 `ScopedResState` 的 NetConn DNS 读取能力。IP 地址变化触发刷新，30 秒轮询覆盖 DNS-only/VPN 变化；不因缺少 `/etc/resolv.conf`、`/etc/hosts` 监听而将有效配置作废。
- `scripts/ci-incremental-build.sh` 在 **154/155 分支判断之前**应用这份补丁。155 使用迁移快照后仍执行它，不需要修改迁移快照或其校验和。154 的全新应用脚本也已接线。
- `shell_secure_dns_ohos.h` 为 `getPrefs` / `setPref` 增加字符串键 `secureDns`，内容为 JSON。依次检查 Local State 注册、两项策略权限、模式、长度及所有 DoH 模板，全部通过后才改值；使用 Chromium 原生 DoH 解析器。严格模式必须有服务地址；拒绝 HTTP、用户凭据、片段；从严格模式退出时先改模式，进入时先设地址。
- ECH 的 TLS 加密、HTTPS RR 解析、重试及证书验证继续使用 Chromium/BoringSSL 实现，不硬编码 ECH 公钥、外层 SNI 或特定业务域名，不绕过证书校验。
- 不强制更换用户 DNS 服务商。自动模式可能回落明文；指定安全 DNS 为严格 DNS 模式，服务不可用时可能解析失败。它不等于“所有网站强制 ECH”。
- Local State 跟随 user-data-dir。同一进程的普通/无痕窗口共用；Lumie 手机一次性无痕目录独立，配置退出清除。系统 NetworkKit/RCP 上传等请求不受此设置控制。

## 外壳协议

```json
{"command":"setPref","key":"secureDns","value":"{\"mode\":\"secure\",\"templates\":\"https://cloudflare-dns.com/dns-query\"}"}
```

读回 `getPrefs { keys: ["secureDns"], requestId }`：`values` 中的 `secureDns.value` 是含 `mode`、`templates`、`managed` 的 JSON 字符串。旧引擎不返回该键，外壳禁用操作。`managed` 只读，写入该字段不能绕过策略。读回确认只是配置保存成功，不是 ECH 连接成功。自动/关闭模式在 Lumie 中保存空模板，避免自动模式误用此前指定的服务。

## 本地检查

```sh
python3 scripts/tests/test-secure-dns-ech.py
node --test scripts/tests/ech-evidence.test.mjs
python3 scripts/tests/test-incremental-patches.py
```

原生配置测试编译并执行生产代码，但对 PrefService、JSON/DoH 解析器、GURL 使用受控桩；验证权限拒绝、校验失败无部分写入、严格模式的更新顺序。它不能代替完整原生编译和系统 DNS/TLS 验收。证据判定测试保证缺少字段、其他域名成功、明确未接受时不会误报 ECH。

补丁已在两版固定上游的 `net/base/features.cc` 和 `net/dns/dns_config_service_posix.cc` 上验证可应用和反向检查。Lumie 的 `node --test scripts/tests/secure-dns.test.mjs` 覆盖协议、旧内核、策略、无效地址、保存读回、超时/异常及并发读取。

## 真机验收（154 和 155 各执行）

1. 分别构建带改动的原生内核及 engine.har。替换 Lumie 的 HAR、清理旧依赖缓存后构建手机和宽屏 HAP；仅构建外壳、保留旧 HAR 不会获得本次内核能力。
2. 在安全 DNS 页保存“指定安全 DNS”和测试环境可达的服务，退出页面再进入、重启后检查读回。使用 `chrome://net-export` 检查生效模式、DoH 查询和 HTTPS RR；配置成功不计为握手通过。
3. 测试域名须通过当前选定 DNS 发布含 `ech` 的 HTTPS（type 65）记录。`ech-test.example` 在 2026-10-26 的公开查询中有该记录，public_name 为 `cloudflare-ech.com`；记录和公钥可能变化，验收时重新查询，不缓存本文中的配置。
4. 重启应用以排除旧连接/会话影响，转发应用的 CDP 端口到本机，然后执行（Node 22+，无需 npm 包）：

   ```sh
   node scripts/check-ech.mjs https://ech-test.example/ http://127.0.0.1:9222
   ```

   脚本新开空白标签页、禁用缓存，再导航目标；只接受目标主框架响应的 `securityDetails.encryptedClientHello === true`。字段缺失、无目标响应、导航错误均不通过，输出证据供日志/抓包关联。脚本会保留新标签，不关闭现有标签。CDP 缺失字段时用 NetLog/抓包和服务端证据联合验收。
5. 抓包核对实际外层 ClientHello 的 SNI 与本次 ECHConfig 的 public_name 一致。仅看到 ECH 扩展或外层名称不等于服务端接受 ECH：必须排除 GREASE，并确认实际协商成功。不能凭“Chrome 能开、App 不能开”确定重置原因。
6. 覆盖无 ECH 记录的网站、服务端 ECH 拒绝/更新、DoH 故障、Wi-Fi/蜂窝/VPN 切换、休眠恢复、普通/无痕目录、只读策略。严格 DNS 服务故障不能被界面显示成保存失败或 ECH 成功；分别记录配置读回与连接错误。DNS-only 变化轮询可能需要最多约 30 秒。

当前本机没有运行原生构建容器，因此尚未完成完整 Chromium 链接、新 HAR 产出和上述真机验收。
