# 渲染进程隔离（P0）

## 状态

2026-10-08：外壳在 Pad 模拟器 HarmonyOS 7.0.0.107 / API 26 上复测
`5d016bcc`：原生密码页成功导入 3 条 CSV；隔离 renderer 已通过资源启动，加载
WebUI 模板与系统字体，保持独立 UID，但创建共享内存时仍访问应用 el2 cache，
因目录不存在而 SIGTRAP。`365333c0` 修复共享内存后端与退出状态观测，
已通过原生编译、HAR 打包，等待外壳复测。
**P0 未解决，默认仍关闭试验开关。**

当前代码中有三种不同的边界，不能混为一谈：

| 边界 | 当前状态 |
| --- | --- |
| 鸿蒙应用沙箱 | 由平台提供，不等于网页与宿主应用之间的隔离 |
| 渲染进程与浏览器进程 | 平板/2in1 通常可分进程；手机根据平台能力查询或旧系统设备类型回退到单进程 |
| 渲染进程的数据、网络、UID 隔离 | 原启动器统一使用 `NCP_ISOLATION_MODE_NORMAL`；本次增加可选的严格试验路径 |

`ohos_chrome_main_runner.cc` 仍带 `--no-sandbox`，OHOS 的
`RendererMainPlatformDelegate::EnableSandbox()` 不安装 Chromium seccomp
策略。删除参数或让此函数返回 true 都不能证明存在隔离。

## 已核实的平台接口

使用本机 `devecocli docs read` 阅读下列文档，并核对 DevEco SDK
`AbilityKit/native_child_process.h`：

- `API参考/Ability_Kit_程序框架服务/C_API/头文件/native_child_process_h/capi-native-child-process-h`
- `API参考/Ability_Kit_程序框架服务/ArkTS_API/Stage模型能力的接口/ohos_app_ability_ChildProcessOptions_子进程启动选项_/js-apis-app-ability-childprocessoptions`

官方接口约定：

- `NCP_ISOLATION_MODE_NORMAL` 共享父进程数据沙箱和网络环境。
- `NCP_ISOLATION_MODE_ISOLATED` 使用独立沙箱；ArkTS 对应说明明确禁止直接网络访问。
- API 20 起提供 `CreateChildProcessConfigs`、`SetIsolationMode` 和
  `StartNativeChildProcessWithConfigs`。
- API 21 起提供 `SetIsolationUid(true)`，只有 ISOLATED 模式下生效；不调用时
  默认仍与父进程同 UID。
- 接口创建的 Native 子进程仍受设备类型限制；这些隔离选项不授予手机创建子进程的能力。
- `NativeChildProcess_FdList` 最多 16 个描述符。

这些是平台文档承诺；尚未通过目标设备的对抗性访问测试确认实际边界。

## 本次试验路径

通过原生启动 JSON 已支持的 `additionalSwitches` 字段明确传入下列数组
（目前 ArkTS 的 `AuraStartupConfig` 类型尚未声明此调试字段）：

```json
[{ "key": "ohos-isolate-renderers", "value": "" }]
```

外壳的启动配置由其自身集成工程提供。当前试验没有新增用户设置或默认开启行为。

1. 启动器在最终参数包含 `--single-process` 时拒绝这个请求，不能用同进程模式
   冒充隔离渲染器。
2. `base/process/launch_ohos.cc` 仅对 `--type=renderer` 使用试验策略。
   GPU 和 utility 进程仍走原路径；这不是全进程沙箱方案。
3. 在运行时解析所需 NDK API，依次设置 ISOLATED 和独立 UID，再调用
   `StartNativeChildProcessWithConfigs`。
4. 任一 API 缺失、配置失败或创建失败，都返回启动失败；没有重试 NORMAL 模式、
   共享 UID 或单进程的降级路径。
5. 配置对象在成功和失败路径上都释放。日志仅记录结果与 PID，不把它标记为安全验收通过。

另外，原先被忽略的 `pre_exec_delegate` 改为拒绝启动：它可能负责建立安全
边界，平台不支持执行它时不能继续忽略。超过 NDK 的 16 FD 上限也提前拒绝。

增加 `OhosRendererIsolationTest.*`，注册到现有 `aura_shell_smoke_tests`，覆盖
API 缺失、配置分配失败、隔离模式失败、独立 UID 失败、启动失败、资源释放和正常
调用顺序。这些测试使用假 NDK 回调，不创建子进程；不能替代系统安全验收。
现有增量 CI 的默认原生目标不包含此测试，需要在 CI 中额外编译并在 OHOS 测试
环境运行该目标。

## 默认启用前必须解决的问题

### 运行资源传递

`WebWindow.prepareRuntimeAssets()` 将 pak、ICU 和 V8 snapshot 解压到
`${filesDir}/chromium-runtime`。原实现把目录路径传给子进程，再通过
`PathService::Override` 创建/解析目录，依赖共享数据沙箱。独立 UID 下不能依赖该路径。

本轮只给隔离 renderer 传入 6 个只读、非空普通文件的 FD：`icudtl.dat`、
`locales/<lang>.pak`、`chrome_100_percent.pak`、`chrome_200_percent.pak`、
`resources.pak` 和 `snapshot_blob.bin`。它们与 Chromium 原有 IPC/共享内存 FD
共同计入 NDK 的 16 FD 上限；不足则拒绝启动，不降级。locale 只允许字母、数字、
下划线和连字符，最终文件以 `O_RDONLY | O_NOFOLLOW` 打开。

子进程校验资源名称、数量、权限及文件类型，复制到 `FileDescriptorStore`；
ICU、V8 和 ResourceBundle 直接消费这些 FD。隔离 renderer 不初始化应用 Profile
目录或 Profile 下的日志，也不再创建/解析主应用的资源路径。未启用开关的进程保留
按路径加载资源的行为。此实现对应当前打包的默认 V8 snapshot，不支持 context snapshot。

启动 FD 不再强制从 1000 起复制，避免依赖 `RLIMIT_NOFILE > 1000`。
失败日志使用实际启用的 Chromium hilog 转发；退出码 70–76 分别标识参数解码、
FD 复制、环境、路径、FD 列表、GPU 启动参数和资源 FD 失败。
日志不再输出完整 argv，也不输出 CSV、密码或页面 URL。

### 共享内存与退出状态

runner 当前 `platform_shared_memory_region_posix.cc` 已有 memfd 实现，但仅对
Linux/ChromeOS 启用。新增 `ohos-memfd-shared-memory.patch` 为 OHOS 启用该实现：
按需分配、设置 `0600` 所有者权限、封印大小，并通过 `/proc/self/fd` 取得真正的只读 FD，保留 Chromium 的
Writable/ReadOnly/Unsafe 句柄契约。memfd 无需应用 cache 目录，但只读 FD 重开仍
需要访问 `/proc/self/fd`，必须在目标 SELinux 域复测。`0600` 防止其他 UID 将
收到的只读 memfd 重新打开成可写；不能沿用内核默认的所有用户可读写权限。隔离 renderer 在 memfd 失败
时明确报错，不回退到应用私有临时目录；普通进程保留旧文件后端的兼容回退。

每个进程首次成功会记录 `OHOS shared memory memfd active`。失败分别记录
`memfd_create failed`、`ftruncate failed`、`size sealing failed` 或
`readonly reopen failed`，含 errno。扩展已有 memfd 大小封印/只读映射测试至
OHOS，并增加不存在 TMPDIR 的隔离 renderer 测试；默认构建不执行这些单元测试。

SDK 将退出回调错误 `16000050` 定义为内部错误。OpenHarmony 公开
[`AppNativeSpawnManager::RegisterNativeChildExitNotify`](https://github.com/openharmony/ability_ability_runtime/blob/master/services/appmgr/src/app_native_spawn_manager.cpp)
在同一 PID 已有订阅时输出本次相同的 `register native child exit:<pid> fail`。
这是一条匹配的实现线索，不足以确定该 HarmonyOS 镜像里是谁先注册。

启动器仅缓存注册成功，失败会在成功创建进程后以及后续启动时重试，不解注册外壳
或其他运行时的订阅。同时为子进程打开 pidfd，配合原有 IPC 断连检测查询实际退出
状态；退出后关闭 pidfd，并在后续启动时清理遗漏的已退出句柄。若内核不支持
pidfd，仍使用原有 PID 探测。只有系统回调才能提供真实 signal；pidfd 确认退出但
没有 signal 时记录异常/未知退出（exit_code=-1），不再伪报正常退出。

### 宿主能力与 IPC

逐项核对字体、Mojo、共享内存、图形缓冲区、媒体和 WebRTC 的访问路径。
隔离 renderer 的网页联网应由浏览器/网络服务代理；不能因网页请求失败就重新给
renderer 开放直接网络。WebRTC 等直接网络路径需要单独评估和适配。

UID 分离后的终止、优先级和退出回收也要验证。当前代码有按 PID 调整和终止
子进程的路径，不能假定跨 UID 后仍然有权限；必要时改用平台子进程管理 API。

### Chromium 沙箱与其他进程

OHOS appspawn 隔离不能直接标记为 Chromium Linux 沙箱已实现。
utility/GPU 的权限需求各不相同，要按服务类型设计。仓库 ArkWeb seccomp
参考代码不等于当前 `enable_arkweb=false` 产品已经启用了它。

### 手机

继续通过平台能力查询决定是否能创建原生子进程。若不支持，需要单独研究公开的
可用进程承载方案及平台权限；在找到有效方案前，手机 P0 必须保持未解决状态。

## 真机验收

在测试用 Profile 和协调好的设备上验证，测试只生成无敏感内容的哨兵文件：

1. 父子进程 PID、UID 不同；至少两个不同站点的进程分配符合预期。多进程数量
   本身不能作为站点隔离或沙箱完成的证据。
2. 从 renderer 的原生测试钩子读取/写入父进程私有哨兵文件失败；不能只用网页
   JavaScript 的同源限制来证明操作系统隔离。
3. 显式传入的只读测试文件可读而不可写；所有传入 FD 做权限和用途清单。
4. renderer 直接连接受控测试 TCP 服务失败，而通过 Chromium 正常加载网页成功。
   仅 `socket()` 成功或失败不足以证明整个网络访问边界。
5. 资源、字体、HTTPS、跨站 iframe、PDF、WebGL、音视频、WebRTC、文件选择、
   前后台切换和 renderer 崩溃恢复均通过。
6. API 缺失和强制注入配置/启动失败时没有共享沙箱 renderer 被创建；手机请求
   试验开关时明确失败。
7. 验证终止和资源回收，不留下无法杀掉的不同 UID 子进程。

## 当前验证限制

SSH 不可达，因此通过只读 CI 快照任务取得 runner 实际源码。上游补丁由
[快照 37722941448](https://github.com/miramira8295/chromium-hmos/actions/runs/37722941448)
生成，基线 Chromium revision 为 `743f26418a267dd97c3c1c71d786038ae68cfc8f`。
已校验快照 SHA256，并用 CI 的 `apply_incremental_patch` 函数验证首次应用、
重复应用及结果文件一致性。没有进行本地 Chromium 构建或设备操作。

原生密码页的 CSV 解析器已注册在 utility 主线程。`5d016bcc` 的外壳复测确认
`utility main entered` 到 `main receiver arrived registered=1` 正常，3 条记录
成功导入并在列表可见；普通 UID 的原生导入通路已通过本次设备验证。不能从这次
结果单独确定上一版阻塞的具体原因；30 秒超时/断连恢复的新增单元测试尚未执行。

下一轮重点复测隔离模式的 HTTPS、内置页和 renderer 退出/恢复，日志过滤
`OHOS shared memory`、`OHOS native child tracking`、`OHOS native child pidfd`，
并保留 `OHOS child bootstrap` / `OHOS child startup`。

验证记录：

- `dfbba12` 的 [Adapter CI](https://github.com/miramira8295/chromium-hmos/actions/runs/37719331729)
  已通过公开文件、脚本和固定 Chromium 基线补丁检查。
- 同一提交的 [Incremental build](https://github.com/miramira8295/chromium-hmos/actions/runs/37719331766)
  Build 步骤通过：原生构建 36 步、136 秒，生成 136432724 字节的 `engine.har`。
  本次没有生成签名 HAP，也没有执行隔离单元测试或真机测试。

- `5d016bcc` 的 [Adapter CI](https://github.com/miramira8295/chromium-hmos/actions/runs/37723646963)
  通过；[Incremental build](https://github.com/miramira8295/chromium-hmos/actions/runs/37723646958)
  原生构建 48 步、113 秒，生成 136429502 字节的 `engine.har`。
  [复测版本 build-5d016bcc](https://github.com/miramira8295/chromium-hmos/releases/tag/build-5d016bcc)
  不含签名 HAP。新增单元测试没有执行，也尚未验证隔离网页显示或 CSV 成功导入。
- 本机独立子进程将 `RLIMIT_NOFILE` 降为 64 后，FD 最小值 1000 的复制返回
  `EINVAL`，改为 3 可正确复制同一个文件。此检查说明旧实现依赖高 FD 限额，
  不代表已测得 Pad 隔离子进程的实际限额；新版启动日志会记录该值。

- 本轮上游改动基于 [runner 快照 37725288139](https://github.com/miramira8295/chromium-hmos/actions/runs/37725288139)，
  基线 revision 仍为 `743f26418a267dd97c3c1c71d786038ae68cfc8f`。快照文件哈希和
  补丁首次/重复应用校验已通过，设备验证继续由外壳进行。

- `365333c0` 的 [Adapter CI](https://github.com/miramira8295/chromium-hmos/actions/runs/37727000448)
  已通过；[原生构建](https://github.com/miramira8295/chromium-hmos/actions/runs/37727000473)
  首次原生编译 318 步、255 秒；重试发布的 HAR 为 136430641 字节，ELF build-id 为
  `80abfb6d9c703f91fce5fa33ff6381162bcebb28`。
  最终复测包使用 [build-365333c0](https://github.com/miramira8295/chromium-hmos/releases/tag/build-365333c0)，
  包含 memfd `0600` 权限修正；不要使用此前的中间构建进行隔离验收。
  本轮没有执行新增单元测试或设备测试，未生成签名 HAP。

  发布任务的两次尝试均通过 Build，但 Upload artifacts 失败。第二次已上传
  `engine.har`，符号包与 `SHA256SUMS` 尚未上传；不能把整个 workflow 记为通过。
  GitHub 资产元数据给出的 HAR SHA256：
  `420f8562c54e47fec4662c9bb01cbc6d42ebdbc405b77589abed8c1c9030524c`。
  失败 job 日志接口返回 BlobNotFound，attempt 日志包为空，暂未取得上传失败的
  具体 stderr。以上不影响 HAR 的下载，但该版本的符号附件仍待补齐。
