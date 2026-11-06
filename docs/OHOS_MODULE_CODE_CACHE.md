# 模块代码缓存实验（154 / 155）

`OhosModuleCodeCache` 是默认关闭的 base::Feature，用于比较模块已有缓存时的
后台流式编译与现有主线程缓存消费。不是模块后台反序列化实现，也不是默认优化。

## 路径

补丁 `patches/ohos-module-code-cache.patch` 修改 Blink script_streamer.cc：

1. 原有模块 MIME / Wasm 判断、Service Worker 缓存策略先执行。
2. 只检查 JavaScript 模块且 feature 已开启、响应携带缓存元数据的情况。
3. 使用 V8CodeCache::HasCodeCache 校验当前编码对应的代码缓存 tag；时间戳、
   其他 tag、缺失/损坏的外层元数据继续走原路径。
4. 检查期间恢复 BigBuffer 的完整字节及 hash envelope，不在后台剥离哈希，
   不开始 V8 的经典脚本后台缓存任务。响应头、body pipe、metadata 的所有权
   原样交回正常响应加载器。
5. 使用 kHasCodeCache + null Result，避免 kHasCodeCacheBackground 在后台解码
   feature 开启时要求非空 Result 的断言。
6. 现有 ModuleScript / V8ScriptRunner 非流式路径执行内容校验、V8 缓存消费，
   拒绝时按上游行为从源码编译并清理缓存。路径选择不等于缓存接受。

启用 `--enable-features=OhosModuleCodeCache`；对照/回退
`--disable-features=OhosModuleCodeCache`。关闭应用全部进程后重启生效。
Feature 声明随此 adapter 编译，未加平台预处理以便通用 Blink 测试构建验证；
只有明确 opt-in 才生效，不注册用户界面或修改任何默认策略。

实验选择事件 `OhosModuleCodeCache::PreferCache` 在 v8,devtools.timeline 分类，
含 URL；原始 trace 应作为本地诊断数据处理。通过 v8.compileModule 的
consumedCacheSize / cacheRejected 判断是否实际消费，不用 HTTP 命中代替。

## 集成与验证

154 / 155 的上述上游文件字节相同。实验曾通过 apply-adapter.sh 的干净安装和
ci-incremental-build.sh 的共享增量入口接入；155 在完整 migration snapshot
之上应用，不修改快照及其 digest。该入口现已撤回，当前补丁仅作为实验材料
保留，不会由正式 CI 自动应用，详见下文“CI 恢复状态”。

补丁内的 OhosModuleCodeCacheTest 共 9 项 × feature 开/关：普通缓存、哈希头
保留、不匹配哈希保留、无元数据、时间戳、编码不匹配、截断元数据、截断哈希头、
无效内部元数据。后台 decoder 保持开启，用完整 response handoff 检查所有权
与结果状态。dummy V8 payload 仅测试分支/交接，不证明实际缓存被接受；
是否编译/执行这些 Blink 测试须查本次验证报告，不能把补丁应用成功当测试通过。

Lumie `scripts/diagnostics/cache-fixture.mjs` 提供固定的经典/模块脚本及执行校验，
`script-cache.mjs` 采集分支、编译、消费/拒绝、线程计时、CDP Performance 与堆内存。
真机以相同 native 构建做 A/B/A；分别覆盖无缓存、缓存生成、重复消费、内容更新，
再测 MDN / React。保留网络、扩展、JIT、签名、进程设置，避免同时改变多个因素。
主线程同步反序列化可能增加阻塞，后台事件也会嵌套，不能直接相加宣称收益。
尚未得到真机证据之前，保持默认关闭。

原始上游依据为 154 b510e9d7cd3a2fbd78d0ddc42234103206c5f78d /
155 f043649a0ef1fd96e48d56ddbfc8a4a99aa2ca37 的 script_streamer.cc、
v8_script_runner.cc、script_resource.cc 和 cached_metadata.cc。

## 2026-10-28 真机结论

155 手机单个完整 B 页面明确消费 12 个模块缓存并通过脚本执行校验；恢复 A 后
模块消费恢复为 0。B 后续出现 GPU 重启/CDP 超时，样本不足，不能归因或量化收益。
默认保持关闭，手机已恢复测试前内核。两版 native 构建通过；后续 155 CI `wzfp5525lq` 已独立编译包含新增 18 个配置的
script_streamer_test.cc 对象文件（OHOS ARM64，沿用 unit_tests 编译参数），
但尚未链接/执行这些 Blink 用例。完整证据：
`<local-artifacts>/module-cache-ab-20261028/README.md`。

后续受控前台 A/B 各约 75 秒，GPU PID 均保持不变；B 再采 3 页均消费 12 个
模块缓存，恢复 A 后 3 页均为 0，全部执行校验通过。本轮未复现 GPU 重启；
这不代表已修复上一轮故障。B 同步收尾 wall 更高，暂无整体收益结论，默认仍关闭。
后续证据：`<local-artifacts>/module-cache-gpu-followup-20261028/README.md`。

## CI 恢复状态（2026-10-28）

实验入口已撤回：apply-adapter.sh、ci-incremental-build.sh 和 native-build.yml
均与当前分支 HEAD 一致。补丁仅保留为本地实验材料，不会随原有 CI 自动应用。
154/155 临时构建副本的实验工作流也已恢复；共享缓存已在先前清理构建中
撤销补丁、重编译后保存。恢复凭据：
`<local-artifacts>/ci-restore-20261028/verification.json`。


## 原生缓存消费者回归补充（2026-10-28）

`patches/ohos-module-cache-consumer-tests.patch` 只增加测试，不改运行时实现，
不由正式 CI 自动应用。与现有 handoff 补丁不重叠；154/155 锁定版本的
`v8_script_runner_test.cc` 相同，两版均可直接应用。

四项 `OhosModuleCodeCacheConsumerTest` 使用真正的 V8 模块代码缓存，分别覆盖：

- 有效 payload 被明确接受，保留缓存并正确执行。
- 保留 Blink 元数据 tag，仅损坏 V8 头；要求明确拒绝、请求清理持久缓存并执行源码。
- 截断 V8 payload；要求同样拒绝、清理和源码回退。
- 改变源码长度；要求拒绝旧缓存，并将导出值和全局副作用从 42 更新到 43。

测试通过 tracking handler 检查 `DidUseCodeCache` 的拒绝标志和
`kClearPersistentStorage` 请求，并执行模块检查 Promise 状态、导出值与副作用。
测试中的持久存储提交为空实现，不写浏览器真实缓存；因此只能证明清理请求，
不能据此声称已验证磁盘删除、HTTP 源码哈希、后台 handoff 或端到端缓存损坏恢复。
后两类分别与原有 handoff 用例、真机源码重验证互补。

`python3 scripts/module-cache-test-syntax.py` 复用现有编译数据库的 OHOS ARM64
参数，通过 Clang VFS 将测试源映射到临时副本，只做 `-fsyntax-only` 检查。
不对共享源码打补丁，不输出共享对象或依赖文件；报告检查原始源文件哈希不变。
默认输出 `validation/consumer-tests/{syntax-result.json,compiler.log}`。

`python3 scripts/tests/test-module-cache-test-syntax.py` 检查输出参数移除、危险命令
拒绝、成功与编译失败时的证据保存和共享源码/对象保护。其通过不代表 C++ 用例执行。

独立 GitHub 验证分支为 `ci/module-cache-validation-155`，不使用 Depot。
编译检查也不等于测试执行；后续须在可运行的匹配环境链接并执行
`All/OhosModuleCodeCacheTest.*:OhosModuleCodeCacheConsumerTest.*`，然后再完成真机
损坏 payload 验收。实验保持默认关闭，尚无整体性能收益结论。


本轮编译验证已完成：GitHub 原生独立验证 run
[40656922582](https://github.com/miramira8295/chromium-hmos/actions/runs/40656922582)
成功，4 项消费者用例以 Chromium 155 的 OHOS ARM64 参数通过语法编译，
共享源码 SHA-256 前后一致。初次编译发现的 span 截断长度类型错误已修正。
4 项 Python 隔离/失败处理回归也通过。以上 C++ 用例仍未链接、未执行；
154 仅核对补丁可应用，没有声称单独编译通过。完整证据保存在
`<local-artifacts>/jit-development-20261028/README.md`。
