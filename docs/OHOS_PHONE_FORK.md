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
真实 Chromium 154 已在手机跑通网页与 renderer JIT，但稳定性验收仍有失败，见下文。

浏览器通过私有 `SOCK_SEQPACKET` 控制通道发送编码参数与 `SCM_RIGHTS` FD。
服务关闭继承的非标准 FD，重置生命周期信号和信号屏蔽，再根据请求 fork；保留系统运行时
的私有信号处理。收到的 FD 在 fork 前复制到不低于 256、且高于继承 FD 最大编号的位置，
避免与继承运行时缓存的低编号 FD 冲突。手机实测未迁移时通道会在入口握手前关闭，
迁移后通过；具体关闭它的系统库尚未定位。
子进程复用 `ChromiumNativeChildMain` 入口和参数/FD 恢复逻辑。
恢复为 Chromium GlobalDescriptors 时的副本也必须保持上述高编号，不能再次复制回
低编号；官方 appspawn 子进程仍以 3 为最低可用编号。
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

宿主测试使用测试 DSO，只验证服务传输和生命周期；真实 HAP 测试项目在本机
Downloads/ForkChildProbe，结果保存在该项目 `artifacts/fork-server-*` 中。

### 真实 Chromium 154 外壳验收（2026-10-13）

Lumie 外壳 `937cc03e` 使用本分支 `fd5a771f` HAR，CI
[38351864547](https://github.com/miramira8295/chromium-hmos/actions/runs/38351864547)
构建成功。Mate 70 Pro+ / HarmonyOS 7.0.0.109 / API 26，**debug 签名**：

- A：关闭 fork，单进程基线通过。
- B：fork + jitless，网页、交互和多进程通过；一次 renderer 崩溃后的刷新白屏，
  冷启动复测恢复成功，仍有后台 renderer 未连接即退出。
- C：fork + JIT，正式冷启动和崩溃恢复通过；独立 renderer PID 的 trace 记录到
  Maglev 代码生成、优化编译及优化函数安装，确认实际 JIT。首次安装会话仍出现同类失败。

三组 getAboutInfo 均确认 `fd5a771f`。此轮证明了网页运行和 renderer JIT，
**没有通过完整稳定性验收，也没有验证 release 签名下的 renderer JIT**。
原始报告及证据目录：本机 `Downloads/Lumie-phone-fork-fd5a771-20261013`。

B 失败 renderer 58890 在 14:09:55.032 创建，14:10:10.085 出现通道错误，
最终 `channel_connected=0 status=0 exit_code=0`。Chromium 154 的
`ChildThreadImpl::EnsureConnected` 默认 15 秒未连接就以 0 退出，与该现象吻合；
尚需子进程阶段记录确认，不能单凭这个退出码认定启动成功或归因于 jitless。
成功与失败子进程均缺少 hilog，因此缺少启动日志本身也不能当成故障证据。

### 本轮修正与下一轮验收

针对已发现的 FD 恢复隐患，保持 Chromium FD 副本在高编号；shim 无法解析引擎入口
时以 78 退出。新增固定大小的共享内存阶段记录，不依赖子进程 hilog，服务在
`waitpid` 后保存最终阶段并交给宿主。它是诊断信息，不是安全边界或独立的就绪判据。

宿主查询退出状态时会输出（示例，不是已采集到的真机结果）：

```text
OHOS phone fork child exit pid=... stage=connection-timeout detail=15 wait_status=0
```

阶段包括 `forked`、`loading`、`entry`、`native-main`、`descriptors`、`chrome-main`、
`accept-invitation`、`invitation-accepted`、`connected`、`channel-error`、
`connection-timeout` 和 `returned`。`invitation-accepted` 只表示 Accept 调用返回，
`connected` 才表示 Chromium 的连接回调已到达。记录为最新阶段，不是完整事件序列；
`wait_status` 为原始 POSIX 状态，不能直接当退出码使用。

本轮 Linux 126/126 通过，另含服务丢失、动态库缺失用例；验证运行中及已回收子进程的
阶段读取。新增“FD 恢复后系统库关闭缓存低 FD”的回归，临时改回低编号会复现 EOF，
修正后通过。Mate 70 Pro+ 的 Release 探针 HAP 同样 126/126 通过，100 次普通启动
宿主 FD 为 44→44，已回收子进程的阶段与 detail 校验通过；证据为探针目录
`artifacts/fork-server-phone-fd-restore-final.txt`（宿主 PID 44798）。首轮测试 DSO 因 NAPI
宿主库局部符号可见性查找失败，显式解析宿主库后重测通过。
此结果**不能代替 Lumie 白屏故障的复测**。

可重复的 Linux 检查：

```sh
scripts/test-ohos-fork-server.sh
```

外壳下一轮保持现有接入，不增加 fork 或子进程启动 API：

1. 替换本轮构建的 HAR，清理依赖缓存并用 getAboutInfo 核对新提交；保留旧 HAR 和失败证据。
2. A/B/C 保持同样配置，每次结束整个应用再冷启动。B/C 都测试多标签恢复、跨站导航，
   并循环执行测试 renderer 的 Page.crash → 页面重载，确认宿主存活及 DOM/JS 恢复。
3. 特别保留刷新前后至少 20 秒宿主日志，按 PID 对齐 `child created`、`renderer launched`、
   `phone fork child exit` 和 `renderer process died`，附 CDP target/PID、页面事件及白屏截图。
4. 若再次出现 `stage=connection-timeout detail=15`，说明确实到达 Chromium 连接超时；
   若停在 `loading` / `entry` / `chrome-main`，则沿对应加载或启动阶段继续定位。
5. 统计多轮失败次数，检查 FD、RSS、存活子进程和整应用退出清理；一次成功不消除旧失败。
   release 签名的网页/JIT 和长期压力仍需单独验收。

`server ready` 只证明控制通道建立，`child created` 只证明获得 PID，均不表示网页可用。
同 UID 的地址空间分离也不等于权限隔离，不得将本实验当成安全沙箱。
