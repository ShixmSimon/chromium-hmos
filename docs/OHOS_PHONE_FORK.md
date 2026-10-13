# 手机 fork 子进程实验

`experimentalPhoneFork` 是默认关闭的启动配置。开启后，手机/折叠屏尝试通过
提前创建的 fork 服务运行 renderer 和 utility，GPU 仍在浏览器进程内。
这不是官方 NativeChildProcess 在手机上的支持，也不提供 renderer 安全沙箱。

## 开关

外壳在首次创建 WebWindow 前生成启动配置：

```ts
const config = UIManager.buildAuraStartupConfig(CHROMIUM_HOME_URL);
config.experimentalPhoneFork = true;
config.gpuProcess = 'in-process';
config.jitless = true; // 首轮内核验收先排除 JIT 权限问题
AppStorage.setOrCreate('startupConfig', JSON.stringify(config));
```

窗口/折叠状态变化重新生成配置时也要保留同一个开关值。关闭开关后必须结束整个
应用进程并重新启动；它不是运行中切换模式的开关。省略或设为 `false` 时保留原路径：
手机单进程，支持官方子进程的设备使用官方 API。

仅接受 `deviceClass` 为 `phone` 或 `foldable` 的非 headless 启动。以下配置拒绝启动：
`gpuProcess: 'separate'`、显式 `--single-process`、`--type`、
`--ohos-isolate-renderers`，或 Chromium 全局 CommandLine 已初始化。
不能只在 additional switches 中写 `--ohos-phone-fork` 来绕过启动时序。
该内部参数由类型化配置生成，不需要外壳重复添加。

## 实现边界

```text
ArkTS / 浏览器宿主
  ├─ 浏览器 ChromeMain + 进程内 GPU
  └─ 提前 fork 的服务（单线程，拥有并回收自己的子进程）
       ├─ renderer → libnweb_render.so → ChromeMain
       └─ utility  → libnweb_render.so → ChromeMain
```

服务在 `EnsureStarted` 内、Chromium ICU/ChromeMain/工作线程初始化前创建。
此时 ArkTS、系统库以及引擎映射已经存在，因此仍会继承运行时状态和潜在的锁，
不能把它等同于 exec 或 appspawn 的全新进程。没有清理匿名映射的 `munmap` 技巧。
此路径只有完成完整内核的真机测试后才能判断是否可用。

浏览器通过私有 `SOCK_SEQPACKET` 控制通道发送编码参数与 `SCM_RIGHTS` FD。
服务关闭继承的非标准 FD，重置生命周期信号和信号屏蔽，再根据请求 fork；保留系统运行时
的私有信号处理。收到的 FD 在 fork 前复制到不低于 256、且高于继承 FD 最大编号的位置，
避免与继承运行时缓存的低编号 FD 冲突。手机实测未迁移时通道会在入口握手前关闭，
迁移后通过；具体关闭它的系统库尚未定位。
子进程复用 `ChromiumNativeChildMain` 入口和现有参数/FD 恢复逻辑。
参数上限小于 64 KiB，每次最多 16 个 FD，服务最多同时管理 64 个活跃子进程；
Chromium 默认 renderer 上限为 16。

退出状态由服务对自有 PID 执行 `waitpid` 获得，`base::Process` 的查询、终止和等待
通过服务处理，保留正常退出码和致死信号。没有接管应用全局 SIGCHLD。
浏览器正常关闭时服务终止并回收子进程；父进程异常死亡由 `PR_SET_PDEATHSIG`
约束生命周期。服务丢失时将未确认退出的子进程标为未知异常，禁止重建服务或晚期直接 fork。
退出状态缓存当前按 PID 保留到浏览器退出，长期运行的容量仍需后续评估。

服务 RPC 的故障等待上限为 5 秒；即使调用 `WaitForExitWithTimeout(0)`，状态查询
也可能等待一次 RPC。服务失联后的 SIGKILL 回收使用阻塞 `waitpid`，内核不可中断等待
可能延迟退出。这些限制使当前实现只适合实验构建。

## 验证与验收

2026-10-13 已验证：

- Linux：真实服务源码的 125 项检查通过，100 次普通启动的宿主 FD 数不增长；
  独立的服务意外退出、动态库缺失测试通过。
- DevEco API 26 / arm64：同一份服务源码与真实 AbilityKit 结构编译、签名成功。
- Mate 70 Pro+ / HarmonyOS 7.0.0.109 / API 26 / Release HAP：125/125 通过，
  100 次普通启动的宿主 FD 为 44→44。覆盖 IPC、创建服务后才分配的共享内存、线程、
  非零退出、SIGABRT、SIGKILL、被宿主屏蔽/忽略的 SIGTERM、8 个并发子进程和关闭回收。
  这是一次短循环结果，不代表长期无泄漏。
- 增加模拟继承库关闭缓存 FD 的 fork hook 回归；测试 DSO 入口前还有加载阶段进程名，
  普通请求报告共享内存/线程阶段，便于区分传输和子入口故障。临时副本禁用 FD 迁移后，
  Linux 回归重现 EOF 故障；保留迁移的最终代码全部通过。
- 服务源码和修改后的 `process_posix.cc` 通过已有 Chromium 原生构建环境的严格编译参数语法检查（该环境为 150，
  仅补充检查警告和类型转换，不是 154 的完整构建验证）。
- 固定 Chromium 154 修订 `b510e9d7cd3a2fbd78d0ddc42234103206c5f78d`：
  全新主补丁之后、增量 child-exit-observation 补丁之后，新增补丁均可应用。

宿主测试使用测试 DSO，只验证服务传输和生命周期，**不代表 Chromium renderer、Mojo
握手或网页首帧已经通过**。真实 HAP 测试项目在本机 Downloads/ForkChildProbe；结果
保存在项目的 `artifacts/fork-server-*` 中。完整 Chromium 154 编译及设备验收尚未完成；
本机已有的原生构建树为 Chromium 150，不能用它代替本分支版本验证。

可重复的 Linux 检查（需要 Linux C++17 编译器）：

```sh
scripts/test-ohos-fork-server.sh
```

下一阶段必须使用同一次 Chromium 154 构建的引擎和 HAR，在手机开启开关验收：

1. `jitless: true` 冷启动，确认浏览器、fork 服务和 renderer PID 不同；
   日志必须越过 `OHOS child bootstrap entering ChromeMain`，并完成 Mojo 握手。
2. 加载本地确定性测试页，确认首帧、DOM/JS、输入、导航、跨站新标签和 utility 服务。
3. 只终止一个 renderer，确认主界面继续响应，页面报错或重建且无僵尸进程。
4. 循环打开/关闭页面，检查 FD、RSS、子进程数量，以及后台/前台、整应用退出清理。
5. 关闭开关冷启动，回归原手机单进程路径；再单独验收 JIT 权限及可执行内存。

`server ready` 只证明控制通道建立，`child created` 只证明获得 PID，均不表示网页可用。
同 UID 的地址空间分离也不等于权限隔离，不得将本实验当成安全沙箱。
