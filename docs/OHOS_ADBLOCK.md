# Lumie adblock-rust（协议 v1 与 55345175 基线）

> 本文保留 55345175 首版的能力范围和测试记录。当前 Brave 执行路径、资源包与验收边界
> 以 [OHOS_ADBLOCK_BRAVE.md](OHOS_ADBLOCK_BRAVE.md) 为准；外壳协议仍为 v1。

内核接入 Brave adblock-rust 0.13.3，独立于网址防护。所有设备形态、单进程与多进程共用此实现。
当前分支继续为 codex/phone-fork-renderers；不需要切分支或清理 Chromium 构建缓存。

## 接入与边界

- 浏览器 URLLoaderThrottle 检查主文档、子框架及重定向；渲染器 URLLoaderThrottle 在子资源发送前异步检查。
- document 所属子资源在 Service Worker 拦截、网络/磁盘缓存路径前经过 throttle；
  Blink 已复用的内存资源不会产生新请求，规则变更后由用户刷新完整应用。
- 无明确 LocalFrameToken 的独立后台/共享 worker 流量目前跳过；不声称覆盖这些后台请求。
  WebSocket 单独的握手路径尚未接入。扩展/内页/系统和订阅更新请求跳过。
- 原生用户 CSS 样式表覆盖每个 HTTP(S) 文档/子框架，动态 DOM 自动按 CSS 匹配。
  配置变化长轮询更新/撤销 CSS；跨文档检查 DocumentToken，BFCache 恢复重新查询。
- 只声明 network、cosmetic。scriptlet、redirect、CSP、removeparam、procedural/action
  及无法识别的规则计入 unsupportedCount，个人规则遇到这些语法返回错误行。
  预处理条件段跳过，不能将条件规则错误当作无条件规则。不会执行订阅脚本。
- 普通配置写入 Profile/LumieAdBlock.json；无痕继承快照，配置只读，本站覆盖只驻内存。
  例外精确匹配 IDNA hostname（忽略末尾点），覆盖端口但不自动覆盖子域。
- 每个页面最多 100 项域名/类型/来源记录；请求、规则原文不记日志，不保存浏览历史。
- 修改后的新请求采用新快照；更新失败保留旧快照。不会自动刷新或重启应用。

## 构建

`bash scripts/setup-adblock-rust.sh` 安装固定 Rust 1.96.1 和 OHOS arm64 标准库。
GN action 使用 Cargo.lock 和 vendor.tar.gz，`--frozen --offline` 构建
liblumie_adblock.so。单独 DSO 避免与 Chromium 的 Rust 标准库及分配器冲突。
HAR 必须携带该库；stage-runtime-assets.sh 已纳入强制检查与复制。
许可证、依赖通知和完整规则快照随 HAR rawfile/adblock-licenses 分发。

本地引擎测试：`bash scripts/test-adblock-rust.sh`。
外壳测试：在 Lumie 仓库执行 `node --test scripts/tests/adblock.test.mjs`。

## 真机测试（由外壳侧执行）

可控探针：`python3 docs/tools/adblock_probe.py`，手机访问
`http://<电脑局域网 IP>:8108/`。页面给出两行个人规则和逐项预期，覆盖脚本、图片、
iframe、fetch、重定向及 CSS。系统如询问局域网访问权限需正常允许。不要自动操作手机。

1. 替换 HAR 后冷启动，在广告设置确认 supported=true、engine=adblock-rust、ready=true，
   capabilities 为 network/cosmetic，三内置源存在且前两项启用。离线首次启动仍应 ready。
2. 用自有 HTTPS 测试页加载广告脚本、图片、iframe，含一次跳转到广告 URL 的重定向。
   个人规则加入 `||<测试广告主机>^` 和 `<测试页面主机>##.ad-slot`。
   Network 应显示 ERR_BLOCKED_BY_CLIENT，.ad-slot 应隐藏；正常资源继续加载。
3. 关闭本站后，原生确认成功，刷新恢复请求和元素；不同子域仍受过滤；不同端口保持同一例外。
   再开启/关闭总开关确认新请求和 CSS 更新、reloadRequired 提示且不丢表单。
4. 同址刷新、页面跳转、快速切标签、多窗口：统计仅属于当前文档；取消请求不能增加计数。
   主文档被规则阻止也应有计数；后台 worker/WebSocket 不作为本版覆盖通过项。
5. 自定义 HTTPS 订阅：有效列表、HTML 错误页、无效 UTF-8、超 16 MiB、HTTPS→HTTP
   重定向、ETag 304、网络中断。失败保留旧列表；禁用来源不参与匹配。
6. 文件导入后立即删除原目录项仍能完成；错误路径/符号链接/超限文件拒绝。
   个人规则错误返回一开始计数的 errorLine，旧规则继续生效。
7. 无痕配置写入只读；本站例外仅无痕会话生效，普通窗口不受影响。
8. 与现有防钓鱼开关分别验证。视频播放、YouTube、复杂 Canvas 页面单独做兼容性和
   滚动/加载性能测试；不能把规则命中等同于解决视频广告或反广告检测。

尚需真机验证：设备加载新 DSO、首启/订阅性能、真实页面 CSS 效果和 IPC 开销。
