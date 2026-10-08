# 渲染进程隔离（P0）

## 状态

2026-10-08：显式启用的隔离启动试验路径已通过 `dfbba12` 的 Chromium 原生编译
及 engine HAR 构建；隔离单元测试尚未执行，真机尚未验证。**P0 未解决，默认启动行为仍不提供 Chromium
渲染器沙箱。试验开关不应放进发布配置。**

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
`${filesDir}/chromium-runtime`。父进程只把 `resourcesDirectory` 路径序列化给
子进程，子进程通过 `PathService::Override(DIR_ASSETS, ...)` 再按路径读取。
这个启动契约依赖共享文件系统；独立沙箱/UID 下不能假定同样可用，试验可能在
资源初始化阶段失败。

下一步需要核对 runner 实际 Chromium 源码中的资源、ICU、V8 snapshot 和 locale
加载路径，优先复用 Chromium 的只读 FD 传递机制。只传所需文件的只读 FD，不传
Profile 文件、整个应用目录的 FD，也不通过放宽目录权限恢复共享沙箱。需核算 16 FD
预算，以及 HAP rawfile 的 offset/length。

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

本次访问交接文档所列 `root@192.168.8.110` 返回 `Network is unreachable`，
因此尚未取得 runner 当前上游源码，也未操作真机。SSH 不通不妨碍构建：推送到
`hmos-154-adapter` 后会自动触发 Incremental build，以该提交的 CI 结果为准。
当前改动全部位于 overlay 和文档，不需要先修改 runner 上的源码或上游补丁。

验证记录：

- `dfbba12` 的 [Adapter CI](https://github.com/miramira8295/chromium-hmos/actions/runs/37719331729)
  已通过公开文件、脚本和固定 Chromium 基线补丁检查。
- 同一提交的 [Incremental build](https://github.com/miramira8295/chromium-hmos/actions/runs/37719331766)
  Build 步骤通过：原生构建 36 步、136 秒，生成 136432724 字节的 `engine.har`。
  本次没有生成签名 HAP，也没有执行隔离单元测试或真机测试。
