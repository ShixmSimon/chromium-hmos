# 手机冷启动：Binder 根因诊断续记（2026-10-20）

用户要求只定位根因，允许探针日志；未实施业务修复或渲染后端变更。
后续诊断曾临时加入启动等待点，现已撤回；诊断触发的启动保护状态已恢复，详见末节。
当前安装仍为 native `f8949dd4` + Lumie shell `20000c4`。
最新结论见末节：clone 返回子进程时 Binder FD 就已失效，早于 libc
后续处理和所有普通 child 回调；GPU 仍使用陈旧 IPC FD，失败查询触发约
1.2 秒重试。新一轮逐次 FD 取证还确认 FD 10 后被 /dev/hvgr0 复用，
Binder ioctl 误发往 GPU 设备时返回 EACCES。各轮实际重用时序不同，不混用。
后续缓存预热 A/B 实验已通过：保留独立 GPU，fork 前调用 GameAware
元数据访问器可消除已定位的六次 200ms 重试；这不等于修复 Binder，详见文末。

## 新的运行时证据

通过官方签名 LLDB server 附加已启动进程，仅读取内存，随后 detach：

| 进程 | PID | BinderConnector 实例地址 | 缓存 FD |
| --- | ---: | --- | ---: |
| forkserver | 21544 | `0x5b6ac3c000` | 10 |
| GPU | 21567 | `0x5b6ac3c000` | 10 |

两进程 libipc_common 加载基址均为 `0x5b4d0c0000`，该固件
`instance_` 位于基址 + `0x12a10`，FD 位于实例首四字节。
这是实际内存读取，补上“fork 后缓存实例仍存在”的证据；不是实际 ioctl
返回值的跟踪，也没有对本轮 FD 10 的目标做成功的 /proc 读取。
旧 FDTRACE 中 fork 返回前后的 FD 关闭证据来自另一轮，不能冒充同轮数据。

固件文件校验：

- libipc_common.z.so UUID `E84EE7C8-96DA-7562-07F9-D5C9B949952E`，
  SHA256 `a6d459a80cd082c8805fe558f3b5f0feb772e242d6c049fc7810b6af492b63e0`。
- libc.so UUID `328898E2-5967-3984-8588-15D01ECDB9E3`，
  SHA256 `fb89216a4c2784389b16e941797318be275878aa46fb407a0543775d1c784bd0`。

forkserver 普通 pthread_atfork 链表有 10 项；其中 libEGL_impl child 回调
`0x5b65b9f2a4` 的反汇编只设置一个全局 byte 后返回，未见关闭 FD。
这不能排除其他回调、libc 特殊 hook 或内核路径。libc _Fork 中存在一次
close syscall，但尚未确认其参数是 Binder FD，不能据此归因。

## 仍缺的决定性证据

1. clone 返回子进程最早位置时 FD 10 是否已消失。
2. 如果用户态关闭，捕获执行 close/close_range 的调用栈。
3. EGL 系统查询期间实际使用的 FD、ioctl 返回与重试次数。

不得据此改 CloseInheritedFds 白名单、重置私有 IPC 单例、绕过 atfork、
切换渲染后端或放宽成功呈现门禁。

## 调试环境异常与待恢复项

尝试启动阶段附加时，应用 PID 32271 在约两秒后被 appspawn SIGKILL，
未进入正常内核启动；不是新抓到的原生崩溃，不纳入性能统计。
wait-for-launch 留下诊断 lldb-server PID 32842（PPID 1，应用 UID），
本机等待客户端 PID 93437 已终止。后续清理用 platform server PID 38044
也已随客户端 quit 退出。

取消 appdebug、aa detach、aa force-stop、普通启动均未恢复应用；
shell SIGTERM/SIGKILL 与附加该调试 server 均被拒绝。系统日志也记录
appspawn kill 32842 返回 13。当前尚未恢复正常运行，已请求用户允许
重启整台手机以清理调试状态；此前只获准暂停、重启 Lumie，不擅自重启手机。

本轮临时证据目录 `/tmp/lumie-binder-rootcause.kGzREV/`：
`read_ipc_state.py` 为只读 LLDB 脚本；`PhoneForkFdTrace.log` 是旧一轮
PID 20776/21589/21633 的日志，不是文档中 PID 24872 那轮；
`process-mode-before.json` 为调试前备份，恢复后应只读比较，不盲目覆盖；
`hilog-debug-cleanup.log` 保存清理阶段设备日志。内存值原始输出在调试会话
工具记录中，本表为转录，不伪称已有独立的原始 JSON 文件。

## 用户重启手机后的恢复确认

用户自行重启后，设备重新连上；旧 lldb-server 已消失，aa appdebug -g
没有待调试条目。普通启动成功，browser PID 10535，forkserver 10809，
日志确认独立 GPU PID 10853，Graphite 路径；没有再次尝试启动阶段等待附加。
系统 aa -W 的 TotalTime=399ms 仅是 Ability 首帧，不是网页可用耗时。
14:13:26.675 日志确认 restored window presented，foreground_ms=3497；
这次是手机重启后的首次启动，不与之前稳态冷启动做受控性能对比。
截图 after-reboot.png 显示 Lumie 首页、5 标签、没有启动遮罩。
稳定后 process-mode-after-stable.json 与诊断前备份逐字节一致：
starting=false、failures=0、safeNext=false、confirmedVulkan=true。
未写回备份或更改用户数据；调试环境恢复项已完成，根因诊断仍未完成。

## 继续取证：进入 clone 前 FD 仍有效

临时给 shell 增加由 Want 参数控制的 60 秒等待点，以便在引擎首次 fork
之前附加。仅诊断包使用，未提交；已完全撤回 EntryAbility 的改动。

两轮 browser PID 57324、61905，在 libc `_Fork` 的 clone `svc` 指令
`0x5afab4a53c`（libc 基址 + `0x1b353c`）前停住：

- x8=220、x0=17、x1=0，确认处于实际 clone 系统调用入口。
- `fcntl(10, F_GETFD, 0)` 返回 1，证明 FD 10 此时仍有效且设置 CLOEXEC。
- PID 61905 的 IPC 单例 `0x5b9be35800` 缓存 FD 仍为 10。
- 此处已越过 prepare 回调与 libc 的 pre-clone 处理。

该证据与旧轮“子进程 fork 返回后、应用自有清理前 FD 已关闭”的探针共同
缩小了区间，但不是同轮的前后完整闭环，不能据此认定内核或某个 child 回调。
`artifacts/binder-rootcause-20261020/lldb-session.txt` 保存 fcntl 与寄存器输出；
部分 Python print 输出仅在工具会话记录，不在这个文本文件中。

### 未成功取得的证据与诊断干扰

LLDB follow-fork 未可靠停在子进程的 clone 返回点或 close 断点。
第一次 PID 59045 因调试暂停超过应用 forkserver 的 5 秒握手期限，
被父进程清理；日志 errno=110，不是自然启动崩溃。
第二次通过临时注册一个仅含 `raise(SIGSTOP)` 的 parent atfork 回调
避免握手倒计时，仍未取得子进程断点；该回调只是运行时诊断，随进程终止消失。
不能把“没有命中断点”作为“没有用户态 close”的证据。

两次 hitrace 的覆盖不够：包含 sched 的大记录覆盖掉最早启动段；
窄记录只覆盖 GPU 初始化前段。本机可用 tracepoint 列表没有 close/clone
syscall entry/exit，均未捕获关闭栈。不能以这些 trace 证明关闭发生于内核。

### 剩余回调的只读核对

普通运行 browser PID 1426 再次只读附加，结果独立保存于
`artifacts/binder-rootcause-20261020/runtime-hooks-pid1426.json`：

- IPC 单例 `0x5b9bdf4ac0`，缓存 FD 10。
- libc 特殊 GWP-ASan atfork 指针与 syscall hook table 均为 0。
- FFRT child-init enabled=1，FFRT 自身缓存 FFTS FD=-1。
- FFRT 间接 deinit 指针解析到 `libffrt_acc.so!DevFFTSDeinit`。
  该函数反汇编确实含关闭自身全局缓存 FD 的路径，尚未读取其对应缓存值，
  不能排除或认定它关闭 Binder；FFRT 自身 FD=-1 也不代表间接回调无关闭行为。
- libdeviceinfo child 回调实际只有 RET；libEGL_impl child 回调只设置全局 byte。
  最近符号名可能误导，判断依据为指令，而非 LLDB 显示的最近符号名称。

这次读取在普通启动后，不是 fork 前同一时刻，不能外推所有全局值在 fork 时相同。

### 当前恢复与下一决定性取证点

已撤回临时 shell 等待点，重建并安装普通包，部署 Smoke PASS。
native 仍为 f8949dd4，shell 仍为 20000c4，没有加入性能修复。
诊断导致的 starting/safeNext 标记，仅在应用停止时按诊断前 118 字节备份恢复；
未改标签、缓存或其他偏好。普通启动稳定后 `mode-final-stable.json` 与备份
cmp 一致。browser 1426、forkserver 1836、GPU 1865 正常存在，LLDB 已 detach，
aa detach 成功，进程列表没有遗留 lldb-server。

根因仍未闭环。下一探针应瞄准 clone 返回子进程的最早指令及 child 回调边界，
记录 FD 10 首次由有效变无效的位置；如果用户态关闭则取调用栈。
随后核对 EGL→GameAware→BundleMgr 查询的实际 ioctl FD/返回值及重试次数。
不重复无效的宽范围采样，不凭这些证据修改 FD 白名单或私有 IPC 状态。

## 决定性取证：clone 系统调用返回即缺失 Binder FD

### 普通 child 回调边界

临时诊断进程 browser 14612，将现有 10 个普通 pthread_atfork child 回调
逐个包一层日志；保留原顺序和原调用，记录前后 FD 10 的 F_GETFD。
日志使用 async-signal-safe syscall/write，保存和恢复 errno，不修改 IPC 状态。
运行时包装只存在于此诊断进程，进程已终止。

`BinderCallbackProbe.bin` 完整 480 字节、20 条记录，child PID 18163。
进入第一个回调 `je_jemalloc_postfork_child` 前 FD 10 就已失效，直到最后一个
回调结束都失效；包括 FFRT、EGL、libuv 和各 CloseSocket 回调，均不是本轮
FD 首次失效的位置。安装时 `libffrt_acc` 的缓存 FD=3，libc `_Fork` 的
raw-close 缓存 FD=59，也不是 10。普通回调前仍存在 libc 内部处理的空白段，
所以仅凭这一轮不能归因于内核。该轮启动保护状态异常，不作性能样本。

原始数据与对应回调地址见 artifacts 下 `callback-probe-state.json`、
`BinderCallbackProbe.bin`、`decode_callback_probe.py`。

### 直接跨越 clone 的自动记录

恢复本轮诊断前的启动保护状态后，日志确认 browser 21110：
`startup fork=true skiaVulkan=true probe=true safe=false`。
在首次 fork 入口暂停；调用栈为 libc fork → native 引擎地址
`0x5f1184941c` → ArkTS native dispatch。

此处实际读取：IPC 单例 `0x5b9be355c0` 缓存 FD=10，F_GETFD=1，
对 `/proc/self/fd/10` 的进程内 readlink 明确返回 `/dev/binder`。
libc syscall hook table=0，libc raw-close 缓存 FD=59。

为避免 LLDB follow-fork 失效，仅在该进程内把 libc clone SVC 单条指令
改成跳转到诊断 trampoline。trampoline 首条仍执行原 clone SVC，输入参数
不变；随后在返回 libc 后续指令之前，通过 raw fcntl syscall 记录 FD 10，
保存并恢复通用寄存器、SP 和 NZCV，最后跳回原 SVC 的下一条指令。
没有改成 raw clone 绕过 fork/atfork，也没有改变 IPC 单例或渲染模式。

- 原指令位置：`0x5afab4a53c`，libc + `0x1b353c`。
- 临时 trampoline：`0x5af9b4a000`，188 字节；使用非 fixed mmap hint，
  不覆盖既有映射。安装前后字节由脚本校验。
- 记录 FD 200 在父子间正常继承，child 成功写出记录；不是所有 FD 都消失。
- 该程序只在暂停的诊断进程内改写，正常包及磁盘上的 libc 均未修改。

同一次 clone 的原始 64 字节记录：

| 进程 | PID | clone 返回值 | clone 后第一项 FD 检查 |
| --- | ---: | ---: | --- |
| browser 父进程 | 21110 | 23956 | FD 10 F_GETFD=1，有效 |
| forkserver 子进程 | 23956 | 0 | FD 10 raw F_GETFD=-9，即 EBADF |

child 的检查前没有执行原 libc 的 child 后处理或任何 child 回调。
因此 FD 失效区间已缩到 clone 系统调用边界；不是 Chromium 的
CloseInheritedFds，也不是前述普通 atfork child 回调首次关闭。
不要再找这些用户态回调的 close 栈作为这个首次失效事件的解释。

### 根因判断与边界

已确认的机制是：**该固件的 fork/clone 创建子进程时，Binder FD 不可用，
但 IPC 单例及缓存 FD 10 被内存继承，子进程持有陈旧连接状态。**
设备 IPC 库又没有 PID 校验或失败自动重连；这与此前 GPU EGL→GameAware→
SAMgr 查询及 200ms 重试采样吻合。不能把这描述成“FFRT 误关 Binder”或
未经平台源码核实的“内核 bug”；也尚未确定内核/兼容层具体采用哪种
不继承机制。本轮没有逐次记录 EGL 查询的实际 ioctl 返回及完整重试次数，
所以不把约 1.2 秒静态休眠上限写成已逐次测量的时间。

证据：`clone-probe-state.json`、`clone-probe-install.txt`、
`BinderCloneProbe.bin`、`clone_boundary.S`、`clone_boundary.bin`、
`install_clone_probe.py`、`decode_clone_probe.py`，均位于
`artifacts/binder-rootcause-20261020/`。后续只需补 EGL 的具体 ioctl 错误与
重试时序，或核对平台支持的独立子进程 IPC 初始化路径；不据此私自重置单例。

### 诊断清理

本轮取证过程中，等待点及调试中断触发启动保护，已在应用停止时仅恢复
`mode-before-callback-probe.json` 中的诊断前保护状态；未改标签、缓存或其他偏好。
临时 shell 等待点已从源码完全撤回，普通包已重建，运行时包装、trampoline
和临时映射均随诊断进程终止消失。最终普通包启动与稳定状态复核记录见下文。

普通包重新安装完成，但手机再次自动锁屏，aa 启动返回 10106102，最终启动
复核尚未完成。CLI 将该启动失败标为 Smoke FAIL_CRASH，并非观察到应用崩溃。
不得将此轮写成 Smoke PASS。进程列表确认无 Lumie 或 lldb-server 遗留；
Lumie git status 干净。再次读取 `mode-final-after-clone-restored.json`，
与本轮诊断前的 `mode-before-callback-probe.json` 逐字节相同，均为 118 字节。
普通包构建产物的 modules.abc 中也已无临时 BinderProbe 等待点标识。
待用户解锁后，仅需普通启动并等待稳定期，不再启动调试器。

### 解锁后的最终普通包复核（16:00）

用户解锁后，普通包再次安装启动成功，CLI `Smoke: PASS`。
browser PID 5700，forkserver 5848；16:00:23.487 配置日志确认
`fork=true skiaVulkan=true probe=true safe=false`，16:00:33.800 出现
`OHOS GPU child: settled`。超过 35 秒后再次读出的
`mode-final-normal-unlocked.json` 与本轮诊断前 118 字节备份逐字节一致，
starting=false、failures=0、safeNext=false。没有写回该文件来伪造本次复核。
截图 `final-normal-unlocked.png` 已检查：首页可见、5 标签、没有启动遮罩。
Lumie 工作区干净，无 lldb-server 残留。最终普通运行复核已完成；
这是恢复检查，不是性能修复或提速结论。

## EGL ioctl 与重试实测（16:10—16:17）

为避免日志 FD 重用 10 或干扰 forkserver 白名单，本轮使用匿名共享内存
记录，不在子进程打开日志文件。browser 17264 首次 fork 前安装进程内探针：
只包装 libc ioctl 和 usleep 的原 SVC，原系统调用仍执行一次，保存恢复寄存器、
SP、NZCV；不重置 IPC，不改后端，不修改系统磁盘文件。记录单调时钟起止、
PID/TID、参数、原始 syscall 返回及最多 16 个 FP 返回地址。
调试器在启动继续前已 detach，约 35 秒后暂停读取共享内存。
这是探针下的 syscall 局部耗时，不是未插桩冷启动总耗时或优化前后比较。

共享 arena 共 24 条记录、容量 1260，无溢出，均来自 GPU PID 19443。

- 主线程 TID 19443：12 次 FD 10 ioctl，首次 BINDER_WRITE_READ
  (`0xc0306201`) raw=-9 / EBADF，后续 11 次 raw=-13 / EACCES。
- 同一主线程六次 usleep(200000) 全部正常返回，分别为 200.085625、
  200.135000、200.081719、200.637656、200.095625、201.150781ms；
  **实际休眠合计 1202.186406ms**，不再是静态上限推算。
- 第一次 SAMgr EACCES 距首条 EBADF 36.675ms；六次休眠都紧随同线程
  SAMgr 查询失败出现，最后一次休眠于首条 ioctl 后约 1445.418ms 结束。
  此区间还包含查询及其他执行时间，不能都计为 sleep。
- TID 19596 稍后还有六次 FD 10 ioctl，均为 EACCES，其中一次为
  BINDER_THREAD_EXIT (`0x40046208`)；不得计为另外六次启动主线程重试。

实际返回地址经 GPU 当前模块 UUID/加载地址解析：

```text
eglQueryString → EglCoreInit → EglWrapperLayer::Init/SetupLayerFuncTbl
 → GameAware::DebugLayerInitialize / ResolveGameId / CheckUniversalMetaDataVK
 → BundleManagerHelper::GetMetaDataValue / InitBundleInfo / GetSystemAbilityManager
 → DynamicCache::QueryResult → SystemAbilityManagerProxy::Recompute
 → GetSystemAbilityWrapper → CheckSystemAbility
 → IPCObjectProxy → BinderInvoker → BinderConnector::WriteBinder → ioctl
```

六次 sleep 的返回链均落在上述 GetSystemAbilityWrapper +436 路径；
第一条 EBADF 则来自 EGL LoadLayers → SystemWatchParameter/AddWatcher，
不是 GameAware 的六次查询之一。实测因此确认系统图形层 IPC 查询失败、
重试等待确实占用了约 1.2 秒，但不能说六次查询都收到 EBADF。

GPU 初始化完成后只读内存仍确认 BinderConnector 缓存 FD=10。
补查 /proc/self/fd/10 的进程内表达式未能执行，LLDB 报
“Can't evaluate the expression without a running target”，故没有取得 FD
目标，不能写成 FD 重用、重新打开 Binder 或 SELinux 拒绝的已证事实。
仍缺 FD 10 在 EBADF→EACCES 转变时的生命周期及 EACCES 具体来源。
clone 边界丢失 Binder、继承陈旧 IPC 状态与本轮失败查询相互吻合，但
操作系统内具体失效机制及全部启动时间归因尚未完成。

证据均在 `artifacts/binder-rootcause-20261020/`：
`ipc_timing.S`、`install_ipc_timing.py`、`ipc-timing-state.json`、
`IpcTimingArena.bin`、`ipc-timing-events.json`、`decode_ipc_timing.py`、
`ipc-timing-symbols-pid19443.json`、`resolve_ipc_timing.py`。

### 本轮恢复

Want 门控的临时 30 秒等待点已从 shell 源码撤回，Lumie git status 干净。
普通包重建、覆盖安装后 Smoke PASS；但诊断暂停触发的保护状态导致该次
运行 safe=true，不能冒充正常 fork/Vulkan 复核。已备份为
`mode-after-ipc-timing-protection.json`，在 force-stop 后仅恢复本轮诊断前
`mode-before-ipc-timing.json`（118 字节），随后普通启动成功。
运行时探针及临时映射随诊断进程终止消失，GPU 调试器已 detach/quit，
aa detach 已成功；普通模式稳定期复核见后续记录。

16:15:39.797 browser 25137 日志确认 fork=true、skiaVulkan=true、safe=false，
16:15:50.061 GPU settled。16:17 稳定后读取的
`mode-after-ipc-timing-normal.json` 与诊断前 118 字节备份 cmp 一致，
starting=false、failures=0、safeNext=false；本次只读比较，没有再次写回。
进程列表无 lldb-server，Lumie 源码工作区干净；普通 HAP 的 modules.abc
中未检出 lumieBinderProbeWait/BinderProbe 标识。恢复已完成，不代表已修复性能。

## 逐次 FD 身份取证：失效后复用，IPC 继续误用（17:03）

### 查询对象的补充核对

实际固件 libGameAwareLayer.so 的 GetSystemAbilityManager 调用点
`+0x2f288` 明确设置 w1=401，再调用 SAMgr 的服务查找接口；后续转换为
BundleMgrProxy，失败日志为 get Bundle Manager failed。
CheckUniversalMetaDataVK 的静态调用参数为
`GraphicsAccelerateKit_FusionAware`（与 Vulkan 比较）及
`GraphicsAccelerateKit_Vbmv`（与 true 比较）。本轮阻塞在查找包管理服务
阶段，不能说已成功查询到这些应用元数据；不是网络访问。

### 初步排除与单步诊断的限制

普通运行 GPU 25663 的公开 /proc/status 显示 Seccomp=2、filters=2，
但 browser 25137 和 forkserver 25633 也是相同数值，不能据此说 GPU
刚刚安装了拒绝 Binder 的过滤器。现有 baseline_policy_ohos 源码中
BINDER_WRITE_READ64 为 Allow，其他列举的拒绝分支使用 EPERM 而非 EACCES；
这只是源码核对，不是设备过滤规则的完整反编译。

首次尝试用公开 syscall 和 LLDB 单步核对当前 FD：readlink 指向
/dev/hvgr0，四种无效 FD ioctl 测试（含请求号零扩展/符号扩展）均为 EBADF。
没有打开 FD，没有发送 Binder 消息，没有更改权限。单步服务器有时在 SVC
后的 MOV x21,x0 才停，且首条 F_GETFD=-9 与随后的有效目标不一致，故这组
单步结果只作为线索，不把首条 F_GETFD 作为可靠 FD 生命周期证据。
寄存器与 scratch 栈字节已恢复，随后 detach/quit。
原始文件 `gpu-fd10-raw-check.json`、脚本 `check_fd10_raw.py`。

### 决定性新一轮：每次原 ioctl 前记录 FD 身份

browser 1280 在首次 fork 入口安装进程内探针，随后立即 detach。
沿用匿名共享 arena、原 SVC 执行一次的方案，每条记录额外包含紧邻该调用前
执行的 raw fcntl(10,F_GETFD) 与 readlinkat(/proc/self/fd/10)，不创建日志 FD。
该诊断额外 syscall 会扰动启动调度，不能拿它作为未插桩总启动时间。
读回 71 条记录、容量 744，无溢出；全部来自 GPU PID 2918。

| 阶段/请求 | FD 10 即时身份 | 原 ioctl 返回 | 数量 |
| --- | --- | --- | ---: |
| 早期 Binder 请求 | F_GETFD=-9，readlink=-2/ENOENT，无目标 | -9/EBADF | 9 |
| 后期 Binder 请求（含一次 THREAD_EXIT） | F_GETFD=1，目标 /dev/hvgr0 | -13/EACCES | 9 |
| 正常 GPU 驱动请求 | F_GETFD=1，目标 /dev/hvgr0 | 0 | 47 |

Binder WRITE_READ 请求号仍为 `0xc0306201`，调用链为
BinderConnector::WriteBinder → libc ioctl；THREAD_EXIT 为 `0x40046208`。
47 条返回 0 的是 libEGL_impl 的 GPU 驱动请求，**不是 Binder 查询成功**。
这将 FD 复用与错误请求直接放在同条记录中，不再依赖事后快照猜测。
新轮中首次 /dev/hvgr0 ioctl 在首条记录后约 1457.396ms；在此之前，
EGL/GameAware/SAMgr 六次 200ms sleep 的 FD 身份均不存在，实际休眠合计
1201.748281ms。这一轮六次重试全是 EBADF，和 16:10 那轮的 EACCES
时序不同，不能回填为旧轮的 FD 身份。

GPU 模块解析进一步确认：首次后期 EACCES（记录 31）来自
libEGL_impl → WatchParameter/AddWatcher → IPC；记录 54 来自 Vulkan
CreateInstance → GameService::GetReportProxy → SAMgr → IPC。
后期系统图形附加服务还在用陈旧 BinderConnector，不是只有一个查询点。

### 已确认的根因层级与尚未确认部分

可确认的完整机制：**fork/clone 后 Binder FD 不可用，但系统 IPC 单例与
缓存编号继续被继承使用；图形层发起服务查询，失败后照常同步重试；
随后相同编号又可被 GPU 设备正常复用，IPC 库仍将其当 Binder，导致错设备
ioctl 与 EACCES。** 不是成功等到了服务，也不是网页网络慢。

本轮已排除“EACCES 就说明 Binder 服务权限不够”这种直接推断：发生
这些新轮 EACCES 时目标根本不是 Binder。尚未区分 EACCES 精确由内核
安全钩子还是 hvgr 驱动返回，亦未确认平台 clone 内部为何移除 Binder FD；
因此不宣称特定 SELinux 规则或内核 bug。未改白名单、私有 IPC 单例、
业务代码、渲染后端或系统权限；原生版本未重编译/替换。

证据：`fd-lineage-state.json`、`FdLineageArena.bin`、`fd-lineage-events.json`、
`fd-lineage-symbols-pid2918.json`、`ipc_timing.S`（FDINFO 编译分支）、
`fd_ioctl.bin`、`fd_usleep.bin`、`install_fd_lineage.py`、
`read_fd_lineage.py`、`decode_fd_lineage.py`、`resolve_fd_lineage.py`，
均在 `artifacts/binder-rootcause-20261020/`。

### 本轮清理

Want 参数控制的 30 秒等待点已从 Lumie 源码精确撤回，工作区干净；普通包
已重建。GPU 2918 及 browser 1280 的 LLDB 客户端均 detach/quit，
aa detach 成功。普通包覆盖安装与最终保护状态复核随后记录。

普通包覆盖安装 Smoke PASS，但第一次启动因诊断暂停触发保护而 safe=true；
稳定后保护文件为 confirmed=false/confirmedVulkan=false，已保存到
`mode-fd-lineage-normal-first.json`，不当作正常模式成功。随后 force-stop，
仅写回本轮诊断前 `mode-before-fd-lineage.json` 的 118 字节保护状态并普通启动。
未改标签、缓存或其他设置。普通 HAP 的 modules.abc 中未检出等待点标识，
Lumie 工作区干净，没有 lldb-server 遗留。

最终普通启动 browser 5006：17:05:50.191 配置日志为
fork=true、skiaVulkan=true、safe=false，17:06:00.458 GPU settled。
稳定期后 `mode-fd-lineage-normal-final.json` 与本轮诊断前 118 字节备份
cmp 一致，starting=false、failures=0、safeNext=false；该验证仅仅读取。
截图 `final-after-fd-lineage.png` 已查看：首页可见，无启动遮罩。
正常模式恢复检查完成；没有实施性能修复。

## GameAware 元数据缓存预热 A/B 实验（17:49—17:58）

用户授权“按照他说的实验一下”。本轮只做可撤回的诊断，未落地业务修复。
实验材料独立保存在 `artifacts/binder-rootcause-20261020/gameaware-prewarm/`。

### 实验方法与固件核对

- 仍为 native `f8949dd4` + Lumie shell `20000c4`，手机独立 GPU + Skia Vulkan。
- 经官方签名 LLDB server 附加；沿用 Want 门控的异步 30 秒等待点，保证
  在 browser 首次 fork 前加载/预热。等待点实验后已完整撤回。
- 使用普通 `dlopen("libGameAwareLayer.so", RTLD_NOW)`，不改链接器命名空间、
  系统参数或权限。目标固件 UUID 必须是
  `C0469827-9ACE-D2A8-F933-DDE84D7F7846`。
- 现有导出 `GetMetaDataValue` 位于模块 +`0x11e98`，实际是接受输入/输出
  libc++ string 引用的包装器，并非公开的 const-char* C API。
  通过其正常实现查询 `GraphicsAccelerateKit_FusionAware`；未伪造包信息，
  未直接写 GameAware 缓存或 guard。诊断输入、输出放在独立 scratch 区。
- `GetMetaDataValue` 内部 local-static BundleInfo 位于 +`0x34ea8`，初始化
  guard 位于 +`0x35a38`。**纠正先前静态分析的字段命名：+0x80 是
  versionName，不是包名；实际本固件包名字段为 +0x68。** 函数快路径
  检查 +0x80 的非空字符串；不能再把它解释成 name 检查。
- 使用上一轮相同的 FD-info ioctl/200ms sleep 探针，匿名共享 arena，
  不在子进程创建日志 FD。记录同时包含原 syscall 返回和即时 FD 10 身份。
  两组都保留 GameAware 库引用，区别是是否在 fork 前调用元数据访问器。

### 完整记录的受控对照

| 项目 | 只加载库，不预热 | fork 前预热缓存 |
| --- | ---: | ---: |
| browser PID | 47735 | 56368 |
| GPU PID | 50113 | 56870 |
| arena 记录数 / 容量 | 68 / 744 | 65 / 744 |
| 200ms sleep 次数 | 6 | 0 |
| 上述实际休眠总时长 | 1215.722812 ms | 0 ms |
| 含 GameAware 元数据查询栈的记录 | 13 | 0 |
| 主进程预热访问器耗时 | 未调用 | 3.496719 ms |
| Binder ioctl 返回 EBADF / EACCES | 9 / 9 | 2 / 9 |

两组都无记录溢出。对照组六次 sleep 时 FD 10 均不存在；其调用栈包含
DebugLayerInitialize → ResolveGameId → CheckUniversalMetaDataVK →
GetMetaDataValue → InitBundleInfo → GetSystemAbilityManager。
预热组未再捕获这条元数据链或六次 sleep。统计 Binder 请求时将 arg1
按 uint32_t 还原，避免把符号扩展的负请求号漏掉；返回 0 的 GPU ioctl
没有计入 Binder 成功。

预热前 guard 未初始化，包名及版本均为空。访问器调用后，主进程缓存为
`com.miramira8295.lumiebrowser` / `1.0.0`，guard 已完成初始化。
GPU 56870 随后读取到相同模块基址 `0x5bfb140000`、相同 guard 原始字节
`0101000030dc0000`、相同包名与版本，直接证明该缓存沿 forkserver 路径继承。
元数据键返回空字符串，表示该键没有值，不表示整份包信息查询失败。

17:56:35.451734 GPU 日志确认 Graphite `first native frame presented`、
`phone_broker=1`；17:56:36.486 shell 确认 fork=true、skiaVulkan=true、
restored window presented；17:56:44.884 GPU settled。保留了独立 GPU，
未切换到主进程 GPU 或软件渲染。

### 结论边界与未完成的产品工作

这个固件上，“fork 前只预热 GameAware 元数据缓存”已实际消除了已定位的
约 1.2 秒失败重试段。它没有恢复 Binder：预热组仍记录 11 次失败 Binder
ioctl，包括 WatcherManagerProxy::AddWatcher、ApsManager::ConnectService
及后续图形服务路径。不能把本结果写成“子进程 IPC 已修复”。

本轮有启动门控、调试暂停、库下载和 syscall 探针；不据此宣称整机启动
缩短了 1.2 秒。完整启动收益仍需不带断点的候选实现另行测量，尤其应计入
新增 dlopen/初始化成本。该入口及 libc++ ABI 为非公开实现，当前只确认
匹配 UUID 的固件，不具备跨版本通用保证；未提交或部署常驻预热代码。

### 非完整轮次与脚本纠正

- `load-only-attempt/`：正常加载库成功且缓存为冷；安装器拼接二进制路径
  错误，在修改原 syscall 指令之前停止。不是完整对照记录，未计入表中。
- `warm-startup-only-attempt/`：访问器耗时 5.224844ms，缓存实际已填充；
  旧校验把 versionName 误当包名而停止，未安装 recorder，不计入受控结果。
- `warm-interrupted-recorder-attempt/`：访问器耗时 6.997812ms，正确包名、
  版本已读到；该轮曾进入后台，调试中断使 recorder 的 mmap 表达式返回
  interrupted，未安装完整探针，不计入对照。后续完整预热组在异步等待
  窗口中先装 recorder 再查询，且确认都发生在首次 fork 之前。
- `control-metadata.json` / `control-child-cache.json` 保留原始记录：其中旧
  `bundle_name` 字段实际读取 versionName；空值本身无误。字段更正已写入
  当前 `prewarm_probe.py`，不会追改旧原始证据。汇总也明确标注此区别。

完整证据：`control-{metadata,state,events,child-cache,symbols}.json`、
`warm-{metadata,state,events,child-cache,symbols}.json`、两份 `*-arena.bin`、
`prewarm_probe.py`、`compare.py`、`comparison.json`、`all-runs-hilog.log`。

### 恢复与普通运行复核

临时 EntryAbility 等待点已撤回，Lumie git status 干净；普通包重建并覆盖
安装，Smoke PASS。正常包 modules.abc 不含 lumieMetadataProbeWait、
openWindowStageForMetadataProbe 或等待点文字。两份 native 库与本轮普通
包备份的 SHA256 一致：libweb_engine.so 为
`a2b9f343294f8094427286fac05ecabe5019506e80b0821a261a7b31e22e89bc`，
libnweb_render.so 为
`2f530f9997f2d9682d1736f8aa2db615aae4b37a2a7d7e2923e0703b0c83720a`。

各轮开始前仅在 force-stop 后恢复本轮 `mode-before.json`，消除调试暂停
触发的保护状态差异；未改标签、缓存或其他偏好。最终普通启动 browser
58453，17:57:39.974 fork=true、skiaVulkan=true、safe=false；
17:57:42.061 restored window presented，17:57:50.253 GPU settled。
`mode-normal-final.json` 与本轮开始的 118 字节 `mode-before.json` cmp 一致，
starting=false、failures=0、safeNext=false；最终核对只读取，没有写回。
`normal-restored.png` 已查看：首页可见、无启动遮罩。LLDB 均 detach/quit，
aa detach 成功，设备无 lldb-server 遗留。本轮实验效果随进程结束消失，
当前普通包不包含预热修复。

## 用户测试版预热模块（18:07，后续授权开发安装）

用户随后明确要求把预热代码开发安装到手机。Lumie 新增小型 NAPI/CMake
模块 `products/shell/src/main/cpp/metadata_prewarm.c`，在 EntryAbility
onCreate 的模式选择之后、首个 WebWindow/forkserver 之前调用，仅限
phoneFork && skiaVulkan。没有实验等待点、断点或 syscall 探针；两个
Chromium 原生库的 SHA256 与上节完全一致，GPU 架构保持不变。

应用中裸库名 dlopen 最初报找不到文件，已正常回退绝对路径
`/system/lib64/libGameAwareLayer.so`，未改 namespace 或系统配置。
调用前核对映射 ELF Build ID 和导出入口偏移；不匹配即跳过。系统
libc++ 字符串 ABI 按本固件固定，输入输出及库引用保留进程生命周期，
不会跨运行库析构，也不直接写缓存/guard。失败只尝试一次，继续正常
启动；私有访问器内部崩溃不能靠 ArkTS catch 隔离，仍为固件限定测试版。

18:07:55.439 browser 4919 日志为 `warmed total_ms=2.046 query_ms=0.344`；
55.649 forkserver ready；GPU 5121 于 56.363616 Graphite first native
frame presented，phone_broker=1，随后 settled。Smoke PASS，Node 24
现有 168 项测试 PASS；独立 ArkTS 检查仍只有既有生成 BuildProfile
解析错误，完整构建成功。覆盖安装保留数据，没有卸载。

手机本轮切到其他应用，没有得到有效的完整前台启动确认时长，因此不把
本轮换算成端到端加速，也不声称对安装版重新抓到了 6→0 的 syscall
记录。用户将划掉应用再从图标冷启动做体感测试。实现说明及回退方法位于
Lumie `products/shell/src/main/cpp/README.md`，构建/部署/运行日志保存在
`gameaware-prewarm/installed-candidate/`。本节更新取代上节“当前普通包”
的安装状态描述；上节记录的历史恢复过程保留不变。

## 原生子进程与 fork + exec 最小验证（后续实验）

官方 `OH_Ability_CreateNativeChildProcess` 在此手机返回 16010004，未收到
子进程连接回调。只在应用 manifest 声明 phone 的 `large_screen` 特征，
核对安装后配置后仍返回同一错误；不能凭应用声明获得系统侧启动许可。
没有修改系统白名单、系统参数或设备权限。

fork + exec 实验仅验证能否进入一个最小 ARM64 payload 的 main，不包含
Binder 初始化、Chromium 或 GPU 初始化。成功标准为 payload 标记和退出码
37；下述尝试均未达到这个标准：

| 尝试 | 结果 |
| --- | --- |
| manifest 声明 `executableBinaryPaths` 并提取原生库 | 本地构建、签名成功；手机安装返回 9568449，`check bin file failed` |
| 将 PIE payload 以普通原生 `.so` 打包后直接 execve | 可以安装；文件 mode 0644，execve 返回 EACCES，未进入 main |
| execve 系统 musl loader，以 payload 作为参数 | loader mode 0755、X_OK 成功，execve 仍返回 EACCES |
| 将 payload 复制到应用私有 cache，chmod 0700 后 execve | X_OK 成功，execve 仍返回 EACCES；测试文件已清理 |

fork 本身成功；失败发生在 exec，不能写成“已 exec 后 Binder 不可用”。
X_OK 成功也不保证系统允许 exec。当前未进一步区分执行策略的具体拒绝层，
这些结果只否定本轮尝试的入口，不证明所有 exec 方案在所有固件都不可行。

所有 ExecProbeAbility、测试页、host NAPI、payload 与 manifest 临时配置均已
从 Lumie 产品源码撤回；重新构建通过，并装回实验前的预热测试包，正常启动
Smoke PASS。没有把原生子进程或 exec 实验留在产品启动流程中。

原始证据保留在本地 `artifacts/native-child-ab-20261020/` 和
`artifacts/exec-probe-20261020/`。这些目录及其他 artifacts 已从 Git 提交范围
排除：包含设备日志、用户会话、签名安装包和临时诊断脚本，不随公开报告发布。
