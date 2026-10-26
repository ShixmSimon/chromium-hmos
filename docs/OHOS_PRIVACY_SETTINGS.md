# OHOS 第三方 Cookie 与 HTTPS 优先

154、155 使用同一份桥接头文件 `shell_privacy_prefs_ohos.h`、ArkTS key 和
`ohos-https-first-warning.patch`。该补丁在增量构建的版本分支选择前应用；全量
`apply-adapter.sh` 同样应用，避免 155 使用预备快照时漏掉警告适配。

## 本地验证

- `python3 scripts/tests/test-privacy-prefs.py` 编译生产头文件，模拟 Chromium 边界，
  验证缺失偏好、错误类型/模式、策略与 Advanced Protection 拒写、平衡模式能力及
  HTTPS 设置服务调用。此测试不验证 Chromium 实际编译、JSON 解析、Cookie 执行或 TLS。
- Lumie 的 `node --test scripts/tests/privacy-settings.test.mjs` 覆盖旧内核、错误返回、
  策略、写入拒绝、读回失败、并发和字符串传输。手机/宽屏 HAP 使用当前 HAR 构建。
- 警告补丁可在固定 Chromium 154、155 上应用并反向检查。

## 新 HAR 真机验收（待执行）

必须分别以包含本次改动的 154、155 编译 HAR 测试；旧 HAR 缺少两个新键，页面禁用
是兼容行为，不算网络功能验证通过。

1. 未修改设置时打开两页，确认与内核已有值一致，退出/重启不重置偏好。
2. 使用受控的两站点 iframe Cookie 测试（两个不同可注册域名，第三方 Cookie 为
   `SameSite=None; Secure`）：选择仅无痕阻止时，普通模式允许、无痕默认阻止；
   选择全部阻止后普通模式也默认阻止。使用无例外的新 Profile，并在浏览器
   网络面板/服务端核对第三方 Cookie，不能只看第一方 `document.cookie`。
3. HTTPS 成功站点升级；HTTPS 不可用/超时的受控 HTTP 站点在严格模式出现整页
   警告。点返回不能访问 HTTP；点继续才允许 HTTP，按内核现有规则记住站点例外。
   重新加载、前进/后退均不应空白；证书错误不能因此被静默接受。
4. 支持平衡模式时检查局域网/门户豁免；关闭额外保护后 HSTS 仍有效。
5. 配置策略锁定和 Advanced Protection（可用构建），确认 UI 禁用且直接发命令
   也不能改偏好。测试写入后不匹配或超时，UI 不显示已保存。
6. 手机普通/一次性无痕分别保存并验证退出清理；平板/桌面普通与无痕窗口共享。

本次没有改变默认偏好、清除用户数据或新增自动 HTTP 降级逻辑。


## 预加载网页

154/155 新增 `preloading` JSON 偏好，保留关闭/标准/扩展三档和 managed 元数据。
旧布尔接口 `preloadPages` 同步补注册与策略检查，已启用的扩展模式不被 true 降档。
`ohos-shell-preloading.patch` 声明桥接使用的 `//chrome/browser/prefetch` 直接 GN 依赖，
在全量和双版本共享的增量入口应用。

本地测试扩充到三档读写、读取不写默认值、旧布尔兼容及两条写入路径的策略/注册保护。
测试模拟预加载 API 边界，不验证内核是否实际发起预加载请求。

新 HAR 真机还需验收：已有 extended 打开页面后不变；三档重启保持；策略锁定时 JSON
和旧布尔命令都不能写；普通/无痕设置范围与契约一致。使用受控站点与网络日志验证
预加载请求，区分“允许预加载”和“本次确实预加载”；省电/流量节省、无痕或功能限制
下未发请求不能单独判为保存失败。旧 HAR 页面应禁用并显示不可用。


## 地址和付款自动填充

154/155 新增 `addressAutofill`、`paymentAutofill` 字符串键，带独立 on/off 模式与
策略元数据；保留原来的两个布尔接口。外壳在“设置 → 隐私与安全”提供“地址和联系
信息”“付款方式”两个入口，内含独立开关，复用已有可回退的 SettingSwitchRow。
关闭只改自动保存/填充偏好，已有资料保留；不新增资料编辑/删除接口或付款同步。

本地原生守卫测试覆盖两项独立性、读取不写默认值、两种模式、缺失注册、策略锁定、
错误类型/模式和过长配置；前端测试覆盖旧内核、独立策略、拒写及读回失败。
测试模拟 Chromium API 边界；HAP 构建使用当前 HAR，不证明新桥接或实际表单填充通过。

新 HAR 真机还需验收：分别切换开关并重启确认；检查另一项、密码保险箱及已有资料
不受影响；受控地址/联系信息和付款表单中核对提示/填充行为；策略锁定时确认 UI
禁用且直接命令拒写；验证无痕保存限制和付款身份验证仍按内核原有规则执行。


## 默认网站权限

154/155 新增 `defaultSiteSettings` JSON 偏好；共用 `shell_default_site_settings_ohos.h`
中的注册表选项校验与有效默认来源检查。页面入口位于“设置 → 隐私与安全 →
默认网站权限”；原“网站权限”继续管理站点例外，修改全局默认不清除例外。

每项 choices 来自 Chromium 的 IsDefaultSettingValid，默认来源不是用户或内置默认
时禁用并拒写。旧 setDefaultSiteSetting 与 JavaScript/弹窗的布尔偏好也共用此保护。
没有新增源文件或改动构建入口；实现位于已有 shell_site_settings_ohos.cc 中。

- `python3 scripts/tests/test-default-site-settings.py` 编译生产守卫、白名单、JSON
  处理与旧命令/布尔路径，模拟注册表和 map，覆盖默认选项、类型/长度检查、
  策略/扩展、缺失注册与相互独立性。此测试不代替真实注册表或内核构建。
- Lumie `scripts/tests/default-site-settings.test.mjs` 覆盖缺键、动态选项、策略锁定、
  写后读回、拒写、超时、并发与 JSON 字符串传输。

新 HAR 真机待验收：分别确认位置、相机、麦克风、通知、剪贴板的默认可选值，及
JavaScript、弹窗、声音等二态；对已有例外和无例外的受控站点分别测试，修改默认
后例外保持。测试系统拒绝应用权限时网页不能绕过限制；管理策略/扩展下 UI 禁用，
新 JSON、旧命令与旧布尔接口都不能暗中修改被覆盖的默认值；重启及普通/无痕范围
按契约生效。HAP 构建使用当前 HAR，不代表新桥接与实际权限行为已经通过。


## WebRTC IP 隐私

154/155 新增 `shell_webrtc_prefs_ohos.h` 与 `webRtcIpHandling` 字符串偏好；外壳入口
为“设置 → 隐私与安全 → WebRTC IP 隐私”。保留 Chromium 四种 IP handling policy，
不覆盖默认值。模式沿用 Blink 常量及 ToWebRTCIPHandlingPolicy/ToString 转换，
默认网络路由两档与禁止非代理 UDP 的行为由现有内核实现。

对照固定源码：renderer_preferences_util.cc 将全局策略和逐 URL 覆盖传入 renderer；
PeerConnectionDependencyFactory::CreatePortAllocator 配置 multiple_routes、
nonproxied_udp 和 default_local_candidate；Chrome 普通网页会执行路由偏好。
读取额外显示逐 URL 列表是否非空，不将全局选中项当作每个网页的最终路由。

- `python3 scripts/tests/test-webrtc-prefs.py` 编译生产桥接和共用 JSON 守卫，模拟
  PrefService、Blink 转换与 JSON 解析边界，验证四种模式、未知旧值、策略锁定、
  缺失注册、过长/错误输入及 URL 规则不被修改；不验证真实 ICE 流量。
- Lumie `scripts/tests/privacy-settings.test.mjs` 覆盖四档、覆盖规则元数据、旧引擎、
  拒写、读回失败与字符串传输；实际构建使用当前 HAR。

新 HAR 真机待验收：分别在 154/155 用受控 RTCPeerConnection 测试收集 ICE
candidates、选中的 candidate pair 和实际 UDP/TCP/代理流量；测试有/无摄像头或
麦克风授权，保留 mDNS 规则；逐 URL 策略命中时确认其可覆盖全局配置。禁止非代理
UDP 模式检查直连 UDP 不发生，同时验证 TCP 回退和通话可用性。不能仅凭候选中
没有明文局域网 IP 就宣称不泄漏 IP 或全部流量已走代理。

确认策略锁定不能改、旧 HAR 不显示虚假默认值、读取不改持久配置、重启保持，
并按契约验证手机普通/一次性无痕及宽屏原始 Profile 范围。


## 网站 Cookie 保存策略

154/155 新增 `shell_cookie_storage_ohos.h` 和 `cookieStorage` JSON 偏好，入口位于
“设置 → 隐私与安全 → 网站 Cookie”。支持 allow、session_only、block，沿用
COOKIES 内容设置与已有的默认规则 provider/注册表保护。第三方 Cookie 页面保持
独立，站点例外和已保存资料不在切换模式时删除。

`test-default-site-settings.py` 扩充为编译生产 Cookie 桥接、共用 JSON 守卫及默认
规则守卫，验证三态、会话模式、缺失注册、策略/扩展、错误输入与其他默认值/站点
例外不变。测试模拟 map/registry 和 JSON API，不验证 CookieStore 清理时机。
Lumie 的 privacy-settings.test.mjs 覆盖三档、缺键、拒写、策略、读回及不发清理命令。

新 HAR 真机待验收：使用受控第一方/第三方站点，核对请求 Cookie 与 Set-Cookie
处理，分别验证允许、阻止、会话 Cookie；与第三方控制组合测试，已有站点例外
仍有效。重启、正常会话结束、强制结束与会话恢复分别测量，不把后台切换/杀进程
当成清理成功。确认切换不立即删已有 Cookie，且不宣称会清除 IndexedDB 等全部
网站数据；需删除使用已有清除浏览数据入口。策略/扩展锁定拒写、普通/一次性无痕
实例范围仍按契约验收。HAP 仍用现有 HAR，实际 Cookie 行为待新内核验证。


## 图片加载

154/155 同步增加 `shell_image_loading_ohos.h` 和 `imageLoading`，Lumie 入口为“设置 → 隐私与安全 → 图片加载”。allow/block 沿用 IMAGES 默认内容设置及共用 provider/registry 守卫，不删除或重写站点例外。旧 HAR 缺键禁用。

`test-default-site-settings.py` 编译生产图片读写桥接，覆盖两态、缺注册、策略/扩展、非法模式/JSON、缺 map，以及 Cookie/其他默认值和站点例外保持不变。前端测试覆盖缺键、策略、拒写、读回和无效模式。

新 HAR 真机待验收：受控页面含普通图片、CSS 背景图及视频，切换后重载并检查图片显示与请求；保留已有例外，验证策略锁定、持久化和普通/一次性无痕实例范围。不把图片默认设置宣称为全媒体或全流量阻断。当前 HAP 仍基于旧 HAR，尚未验证实际渲染/网络行为。
