# 手机 EGL 冷启动调查

2026-10-19，PLA-AL10 / Maleoon 920。调查工作区为 `hmos-154-adapter`
`08a2d3bb` 和 Lumie `main` `96552edf`。手机沿用已安装的诊断包，未换包；
安装包包含 StartupHandoff 和启动阶段探针，但本轮尚未取得其精确构建身份，
不能将设备结果直接标记为上述两个源码提交的验收。

## 已定位的等待

手机初始实际后端为 `GraphiteDawnVulkan`，ANGLE 为 OpenGL ES，GPU crash count 为 0。
原生日志使用负载中的 `mono_us` 配对，不能使用延迟转发的 hilog 接收时间。

| GPU PID | 采集方式 | eglInitialize 墙钟 / 调用线程 CPU | GPU 初始化 | Dawn 初始化 |
| --- | --- | --- | --- | --- |
| 9191 | 未加性能采样 | 1524.880 / 260.460 ms | 1648.785 ms | 103.224 ms |
| 10242 | 未加性能采样，前有快速重启，单列参考 | 1538.007 / 301.430 ms | 1689.281 ms | 136.799 ms |
| 12367 | 99 Hz FP + off-CPU，前轮已稳定 | 1585.142 / 330.240 ms | 1695.353 ms | 94.663 ms |
| 15229 | 99 Hz DWARF + off-CPU，前轮已稳定 | 1541.757 / 302.884 ms | 1662.345 ms | 105.452 ms |

原生 scope 已把主要延迟定位到 EGL 调用内部。调用线程墙钟与 CPU 的差值
不是整个进程的空闲时间；不能排除其他线程工作、调度和驱动等待。

两种栈采样均捕获如下链路；DWARF 关闭 callstack expansion，避免依靠合并栈推断：

```text
eglQueryString
  OHOS::EglCoreInit
  OHOS::EglWrapperLayer::Init / SetupLayerFuncTbl
  libGameAwareLayer.so + 0x1854c ... + 0x2f28c
  SystemAbilityManagerProxy::Recompute
  SystemAbilityManagerProxy::GetSystemAbilityWrapper
    CheckSystemAbility -> IPCObjectProxy -> BinderInvoker
    usleep
```

DWARF 报告中该链路的采样加权时间约 1.180 s，其中包含 IPC 查询、诊断展开和
休眠，不能解释为实测“纯 sleep 1.180 s”。也不能把整份启动报告中的驱动时间
不加区分地累加到 eglInitialize 上。

只读取得设备系统库后，对栈中地址作静态核对：

- `libGameAwareLayer.so+0x2f288` 为 `mov w1, #401`，随后调用系统能力管理器。
  `hidumper -s 401 -a -h` 确认设备上的 SA 401 是 BundleMgr。
- `libsamgr_proxy.z.so+0x21b00..0x21b08` 将参数设为 200000，再调用 `usleep`。
  样本返回地址 `+0x21b0c` 与此位置一致，证实本轮执行到了重试休眠分支。
- 这证明 GameAwareLayer 的 BundleMgr 查询参与了等待；尚未取得此查询的具体
  返回错误码，不能断言 BundleMgr 服务未运行，也不能认定所有延迟都由它造成。

系统库 SHA-256（只读副本留在本地 out，不提交系统二进制）：

```text
libGameAwareLayer.so  81e09deefc294ff99e4bd4628249c012b5ccc342fe6245929798d1fc0f284a95
libsamgr_proxy.z.so   d4334f863db7fd2fa907d46d0c6634ea43c23ba48902de7248346dfb1694327d
```

## 外壳等待与测量限制

调查初期手机已安装包实际使用新的 startup handshake；当时 Lumie 主分支仍是旧的
PageTextRequests / StartupHealth 实现。后续主分支接入和验证见本文末尾，不能把这一历史状态当作当前状态。
PID 12367 / 15229 两轮从外壳 health_start 到 mask_release_requested 分别为
2505 / 2483 ms；后者在恢复文档的 WindowPresented 确认后解除遮罩。
这是日志中的状态变更时间，不是物理屏幕呈现时间。

初始脚本两次重启间隔只有约 6 s，未等待约 30 s 的稳定标记，导致中间一轮
PID 9466 使用进程内回退，eglInitialize 为 65.170 ms。它不属于同后端可比样本，
不能宣称“优化到 65 ms”，也不能据此直接断定手机 GPU 驱动只需 65 ms。
后续两轮采样均等到 `startup stable after session restoration` 才重启。
CDP /json/list 可用也早于 GPU 就绪，不能作为启动完成判据。

## 下一步修复边界

优先核对手机 fork 子进程的系统 IPC 生命周期：`fork_server_ohos.cc` 会关闭继承
的应用 FD，但系统库还可能持有进程快照中的 IPC 单例。这是待验证假设，
需要取得实际查询错误码、缓存 FD 状态及受支持的 fork 后初始化机制。
不能仅凭现有证据保留所有继承 FD、修改系统库、绕过 CFI 或直接禁用系统层。

若可取得系统 EGL 对可选 GameAwareLayer 的正式进程级配置，再做单独对照；
未验证其存在前不加入私有参数。也不在 fork server 中预热 EGL 后继承 GPU 状态。
继续保留 Ganesh Vulkan 回退；本轮未修改渲染逻辑和 CI，没有本地原生编译。

## 本地证据

`out/egl-startup-20261019/` 保存了原始阶段日志、两份 perf.data、FP / DWARF
报告、反汇编摘录与 `stage-summary.json`。性能采样分别为 6 s / 4 s，报告均无
样本丢失。采样结果用于定位，不作为无开销的性能基准。

整个测试期间核对 SILENT、MUSIC=0/mute=1、RING=0/mute=1；维持手机亮屏。
未清数据、未清缓存、未切换 flags、未操作 Pad。前后 CDP 对比确认原有 6 个
page 的 URL 多重集合一致，额外 service worker targets 不算新增用户标签页。

## 公开 Bundle 查询对照探针

新增默认关闭的 `--ohos-startup-bundle-probe`。外壳通过现有
`additionalSwitches` 传入；原生启动器只向 GPU 子进程转发，不进入 renderer。

- browser 查询在 early fork server 创建之后发起，避免诊断预热污染 server 快照。
- fork-gpu / native-gpu 在恢复启动参数后发起。同一进程最多一次。
- 查询使用 SDK API 20 的 `OH_NativeBundle_IsDebugMode`，动态加载公开 C 符号；
  不使用私有 C++ ABI，不打印应用信息、指纹、token 或查询得到的 debug 值。
- [上游公开实现](https://github.com/openharmony/bundlemanager_bundle_framework/blob/master/interfaces/kits/native/bundle/src/native_interface_bundle.cpp)
  对该 API 调用 `BundleMgrProxyNative::GetBundleInfoForSelf`。它用于检验公开
  BundleMgr 业务调用，不等价于直接取得 SA 401 proxy。
- 日志 `OHOS BundleIpcProbe` 带 role、pid/tid、mono_us、加载和 API 可用性、
  api_ok、api_errno、loader_errno。API 只返回 bool；errno 并非 Binder 返回码，
  因而明确输出 `binder_status=unavailable`，不能把 false/errno=0 标成成功。
- 查询在独立线程运行，调用方不等待。另一个限时观察线程在 1000 ms 后仍未完成
  时输出 deadline；这不取消系统调用，迟到结果仍会记录。没有自动重试。
- 默认不开启时不加载库、不启动探针线程。开启时可能与系统 EGL 查询竞争，
  仅作诊断；启动性能必须另采关闭探针的同版本基线。native-gpu 的入口本来已先
  执行 EGL 探测，不能把它解释为首次 EGL 之前的系统查询。

本轮探针不重置 IPC、不保留额外继承 FD、不创建或预热 EGL。原始 Binder
错误码仍需系统日志或专门追踪获取；不伪造接口无法提供的字段。

### 首轮对照（61e2308a 的 CI 产物）

- Pad GEM-W00：browser 查询成功，1274 us；native-gpu 查询成功，1763 us。
  当前 Pad 的 ANGLE 为 Vulkan，单次 EGL scope 为 100123 us。
- 手机 PLA-AL10：fork-gpu 的公开 API 查询失败，1393346 us，api_errno=9；
  同轮 EGL scope 为 1551751 us。errno 9 是该公开调用后的线程状态，不能当成
  原始 Binder 返回码，也不能仅凭它证明所有查询失败都由同一 FD 导致。
- 首轮 browser worker 可能在 Chromium 日志转发安装前完成，导致手机父进程
  日志缺失。已将 browser 探针移到安装日志转发之后，仍晚于 early fork server
  创建。需要新 CI 包补齐父进程记录；缺日志不算查询失败。
- Pad 系统子进程成功已排除“只有主进程可以查询”的笼统结论。手机 fork 状态
  与正常 native child 的差异仍是重点；此轮没有重置 Binder 或修改系统层。

后续 FD dump 显示手机 browser 有 `/dev/binder`，fork GPU 没有；Pad 正常
native GPU 有 `/dev/binder`。为区分旧连接缓存与新连接权限，探针在公开 API
结束至少 5 秒后增加一次 `open("/dev/binder", O_RDWR | O_CLOEXEC)` / close，
独立记录 `phase=fresh_binder` 的成功与 open_errno。它不发送 ioctl、不构造
proxy、不替换系统 IPC 单例中的 FD；不能把“打开成功”解释成 SA 查询已修复。
这是显式诊断开关的一部分，普通包不会执行。


## 2026-10-19 手机与 Pad 对照结果

以下为新诊断产物 `2cccb7c37f1360aa7a856fbf9407cc9db19305d8`，
ARM64 CI `39264709757` 和 adapter CI `39264709479` 均通过。
HAR 下载后校验发布的 SHA256SUMS；外壳只装配并签名 CI 内核，
HAP 中两份原生 so 的哈希均与 HAR 一致，没有本地编译内核，也没有改 CI 文件。

| 设备 / 进程 | IsDebugMode 结果 / 耗时 | 延迟 fresh Binder open |
| --- | --- | --- |
| 手机 browser PID 14968 | 成功 / 5314 µs | 成功 / 554 µs |
| 手机 fork GPU PID 15206 | 失败，API errno=9 / 1470887 µs | 成功 / 736 µs |
| Pad browser PID 58729 | 成功 / 5199 µs | 成功 / 365 µs |
| Pad native GPU PID 59006 | 成功 / 3152 µs | 成功 / 292 µs |

手机 fork GPU 能重新打开设备节点，排除了该时刻 open 本身被拒绝的解释；
这不证明随后的 ioctl、SA 查询或远程对象访问得到授权，也不是修复。
结合先前 FD dump 与系统查询等待，下一步应验证受支持的 fork 后系统 IPC
初始化方案，而不是直接替换 cached driverFD。当前仍未取得系统私有查询的
原始 Binder 返回码，不能将 `api_errno=9` 当作该返回码。

本轮安装的 Lumie main `96552edf` 仍只调用 legacy PageText 健康检查。
因此当前内核中的 window-scoped StartupHandoff / resumed-frame gate
没有通过该外壳完成验收，不能把视频、恢复或截图通过扩大为该握手已验证。

### 同轮渲染回归验收

测试设备为手机 PLA-AL10 / Maleoon 920 与 Pad GEM-W00 / Maleoon 935。
61e / b5 / 2ccc 三次提交只增加默认关闭的诊断，不改变渲染实现。
渲染功能检查从 61e 开始，2ccc 补做最新产物、最低亮度、YouTube 与普通包检查；
不能将较早产物的每项长测称为在最终产物上完整重跑。

| 范围 | 手机 | Pad |
| --- | --- | --- |
| Graphite H264、Main10 SDR/PQ、VP9 软件路径 | 通过，另测 HEVC 4K | 通过；Ganesh 另测 5 种来源含 4K |
| NativeBuffer / Dawn 导入、metadata、读租约与释放跟踪 | 短跟踪通过；长期跟踪收尾失败不采用 | 短跟踪通过；长期跟踪只有部分尾段 |
| WebGPU HTMLVideoElement / VideoFrame 外部纹理 | 四种来源通过 | 四种来源通过 |
| Canvas / WebGL / WebGPU / worker 的 P3、HDR 像素 | 接口、原生截图通过 | 接口、原生截图通过 |
| 10 分钟固定 PQ1440p60 播放 | 无 GPU / 浏览器重启；4514 / 37848 帧丢失 | 无 GPU / 浏览器重启；228 / 38751 帧丢失 |
| 全屏、seek、换源、保留 VideoFrame、30 秒后台 | 通过 | 通过；另有真实 HDC 触摸 HDR 全屏保持 15 秒 |
| 系统亮度下限 1 下 SDR 后台恢复 | 四轮通过 | 四轮通过，HDR 退出后再四轮通过 |
| 最低亮度 HDR 退出 / SDR 灰阶 | 十轮全屏与后台恢复后截图通过 | 实际触摸 HDR 全屏后截图通过 |
| TreesInViz 实际活动 | trace 有 update / draw，原设置开启 | 开启后四来源通过，645 次 update / draw；最终恢复关闭 |
| Graphite GPU 主动崩溃回退 | 同 browser、换 GPU、Ganesh Vulkan、SDR 截图通过 | 同样通过 |
| YouTube 静音播放与崩溃回归 | Graphite 通过 | GaneshGL / GaneshVulkan / Graphite 均通过 |
| WebNN float32 Add | 上下文创建成功，但驱动拒绝图，未通过 | 同样未通过；五种形状均被拒绝 |

十分钟观察中 GPU FD 在有限范围内变化；GPU PSS 手机约 351–588 MiB、
Pad 507–770 MiB，未观察到单调增长。这不是长期无泄漏证明。
手机电池温度由约 39°C 升至 43°C，达到脚本 44°C 停止线前测试结束。
用户已允许现阶段 HDR 掉帧；以上通过只针对稳定播放，不是性能验收。
色彩截图为系统截屏映射结果，不代替屏幕 P3 色度、PQ 曲线或峰值亮度实测。
YouTube 播放推进不代表该站点分配了 HDR 格式。

WebNN 失败出现在 `OH_NNModel_GetAvailableOperations` 的能力判断阶段，
不能宣称二十次 dispatch 数值验证通过；该实验保持默认关闭，最终恢复关闭。
未以 CPU 执行伪装 NPU 成功，也未放宽硬件能力判断。

最低亮度测试的初次错误截图被设置面板遮挡，另有普通包截图被控制中心遮挡；
这些原始失败记录保留在 out，作为测试工具失效，不作为色彩缺陷结论。
修正关闭覆盖层后，用独立标识重新执行检查。
所有原始日志、脚本、逐项 JSON 和安装哈希留在
`out/pad-validation-20261019/` 与 `out/phone-validation-20261019/`。


## 后续只读调查：Binder 关闭早于 Chromium 清理

从手机普通包取得已有 `PhoneForkFdTrace.log`，没有唤醒设备、重启应用或换包。
同一轮 browser PID 24872、fork server PID 25126、GPU PID 25159：

| 阶段 | FD 10 |
| --- | --- |
| browser-before-server-fork | `/dev/binder`，F_GETFD 成功 |
| server-after-fork | 已关闭，F_GETFD errno=9 |
| server-before-cleanup | 仍关闭 |
| server-after-cleanup | 仍关闭 |
| GPU chrome-main | 仍关闭 |

`server-after-fork` 的记录在 `fork()` 返回后、`close(sockets[0])` 与
`ServerLoop` 之前；只有三个原始 FD 此时消失，不能归因于后面的全量清理循环。
因此“修改 CloseInheritedFds 以保留 Binder”无法保留这个已经关闭的连接。
关闭发生在 fork 的 runtime / 回调 / 内核路径中；尚未确定其中哪一层执行，
不能擅自绕过 pthread_atfork 或换用 raw clone / _Fork。

进一步只读取得设备实际 `libipc_common.z.so`、`libipc_core.z.so` 和 `libc.so`，
不是仅用 OpenHarmony 上游源码推断该固件实现：

- BinderConnector::GetInstance 在 instance_ 非空时直接返回，没有 PID 校验。
- IsDriverAlive 只判断缓存 FD 数值是否非负，不调用 fcntl 检查描述符是否仍打开。
- WriteBinder 对缓存 FD 调用 ioctl，失败时返回负 errno；它不会自动新建连接。
- 这解释了为何连接缓存可能存活而底层 FD 已失效；尚未读取该进程的缓存实例
  或跟踪具体 ioctl 返回，不能把静态分析提升为每次系统查询错误码的直接证据。

设备 SAMgr 查询循环从 -6 计数，代理为空时每轮 usleep(200000)，最多六次
休眠预算约 1.2 秒。之前采样命中该分支；这是静态上限与采样定位，不能宣称
已经逐次测得完整六次纯睡眠时间。它与最终普通包 EGL 1.692062 秒的总墙钟
吻合，但 EGL 其余调用开销仍单独存在。Pad 最终 GPU 初始化为 0.106201 秒，
应另查外壳健康检查和恢复文档的时序，不能套用手机等待结论。

现有外壳 PageTextRequests 的 4000ms 是无回复时的超时，不是每次启动固定
休眠四秒；StartupHealth 还会每 500ms 重试，且只在 modeProbePending 时运行。
下一步外壳可以接入内核已有 window-scoped StartupHandoff，按 renderer ready
与恢复文档首帧分别确认。需要验证窗口归属、取消、旧内核兼容、后台恢复与
遮罩释放顺序；不能为缩短时间提前宣告 ready。

新增证据：`binder-fork-close-summary.json`、`fork-ipc-device-analysis.json`、
三份 `binder-*-device-disassembly.txt` 与 `PhoneForkFdTrace-final.log`。
设备库 SHA-256 在上述 JSON 中；原始二进制和应用日志只保存在本地 out。
本轮未修改渲染路径、系统配置或 CI，没有进行本地原生编译。

## 2026-10-19：外壳接入、窗口隐藏与 ANGLE Vulkan 对照

Lumie main 已推送 `9e85046`、`ab9ac8d`、`04876e2`：外壳接入内核已有
window-scoped StartupHandoff，按 token / requestId 校验 ready 与恢复文档
presented；只有 presented 才释放启动遮罩并确认模式，稳定标记另计前台时间。
旧内核的文本兼容仅在尚无 native ack 时启用，不作为首帧呈现的证明。

真机早期 Home 测试先后暴露 UI Monitor 延迟、以及 AbilityManager 仍报告
FOREGROUND 而桌面已显示的情况；仅 Ability 生命周期计时不足。
`04876e2` 订阅窗口 SHOWN/HIDDEN/DESTROYED，挂载后读取 isWindowShowing，
以 Ability 前台且窗口未隐藏为累计时间条件；焦点变化不等同后台。
45 项相关宿主测试通过。Pad 新包实测启动后立刻 Home，40 秒后
safeNext=false、failures=0；切回后才出现 ReadyAck / WindowPresented，
随后 starting=false。前两次失败日志保留；仅在应用停止时恢复本轮测试前
process-mode 记录并读回核对，没有清理用户标签、缓存或设置。

Pad 固定本地恢复页 `startup-handoff.html` 单次启动：ReadyReceive 1.705s、
ReadyAck 1.867s、DocumentSubmitted 1.994s、WindowPresented 2.006s。
原有首标签为另一端口 localhost 测试页，不能拿它的恢复等待时间充当受控的
网络条件，也不能把外壳等待全部归于 EGL。恢复后 HEVC Main10 SDR/HDR
与 SDR 像素检查通过，GaneshVulkan 稳定，原八个页面保留。

手机同一 CI 内核另做诊断包，仅追加现有 `use-angle=vulkan` 开关：

| 路径 | EGL 墙钟 | EGL 线程 CPU | GPU 初始化 |
| --- | --- | --- | --- |
| 标准 ANGLE GLES | 1.502800s | 0.254354s | 1.638032s |
| 诊断 ANGLE Vulkan | 1.466665s | 0.246621s | 1.527820s |

这是各一轮诊断，外壳客户端版本也不同，不能作为统计性能验收。
实际 GL implementation 确认 angle=vulkan，GraphiteDawnVulkan、独立 GPU
和零崩溃状态保留，但约 1.5 秒 EGL 等待没有消失，不据此更改默认 GLES。
诊断路径七组 Canvas / WebGL SDR/P3/HDR 原生截图色块与 API 检查通过；
H.264、HEVC Main10 SDR/HDR、VP9 HDR 的 HTMLVideoElement / VideoFrame
到 WebGPU 短时导入均有非空像素、无 device lost / uncaptured errors。
这些检查不证明长期稳定性、面板峰值亮度或所有链路零拷贝。

手机恢复标准 GLES 包后，同样的启动后 Home、后台等待 40 秒、切回测试通过：
后台未设置 safeNext，首帧确认仅在切回后发生，稳定期后 starting=false。

上述包只组装外壳；原生库严格沿用 NativeCI 39264709757 的 2cccb7c3，
两份 so 均逐字节哈希核对。没有本地内核编译或 CI 文件改动。
原始失败、模式回滚读回、逐项真机数据和安装元数据保存在
`out/startup-handoff-20261019/`；手机色彩与导入结果另在
`out/phone-validation-20261019/phone-angle-startup-*`。

最终恢复手机标准 GLES 包，GraphiteDawnVulkan / 独立 GPU / 零崩溃确认，
两设备 Main10 SDR/HDR 与 SDR 恢复短测通过，原手机六页和 Pad 八页保留。
Pad 已停止保活并锁屏熄屏（SLEEP、display State=0）；用户最新要求手机
不要锁屏，故手机保持亮屏静音，有限时保活仍运行，测试媒体已停止。


## 诊断撤除与修复边界

应用侧可修复的启动确认和后台计时问题已修复，并在两设备验证。
手机 EGL 内部的系统查询等待仍存在；当前结论是受已验证证据限制的定位，
不是把整个约 1.5 秒都解释为 Binder 休眠，也未确定 fork 内究竟哪个回调关闭 FD。

再次核对本机 API 26 SDK 的 IPCKit：ResetCallingIdentity 只恢复调用者身份，
不是重置 Binder 连接；没有公开的 fork 后连接/进程骨架重建入口。
设备 BinderConnector 的缓存行为和首个 fork 后 FD 记录不支持“仅调整自己的
FD 清理白名单”这一修复。不能将调用私有析构器、重写实例内存或跳过 libc
fork 回调作为可交付修复。现有受支持子进程 API 还受设备能力约束；不能依据
Pad 支持就改写手机进程路线。保留当前设备适配和默认渲染后端。

撤除范围：base 计时头及全部调用、BundleMgr 诊断工作线程及命令行转发、
每次启动的 PhoneForkFdTrace 文件生成/FD 扫描、Skia/Dawn FD 钩子、Dawn
vkCreateInstance 前的额外 layer 枚举。错误日志和正式启动健康保护保留。
Dawn 的生产视频导入、布局、Tint 修复未改动。

遵守“不改 CI”约束，现有两个诊断补丁路径继续作为迁移入口：内容只在原插入点
保留兼容注释，以使持久构建树先撤回旧探针，再应用新版本。验证覆盖旧版本
不能跳过迁移、迁移后原文件仅多注释、干净树可直接应用；没有空补丁或未定义
探针函数。HDR 补丁中的 FD 调用也已移除，它沿用既有撤回/重叠流程。
构建脚本与 workflows 均未改动。Linux 宿主 fork transport 测试 127 项通过，
包括 RELRO 原始 FD 保留、fstat EACCES、子进程丢失和退出清理；没有本地
编译 Chromium。清理版本仍需推送触发 CI 后再验证产物和真机。

## 连续启动复核发现：后台首帧使 GPU 失败标记遗留

清理提交等待 CI 时，固定本地页冷启动实测出现 GaneshGL / inProcessGpu=true，
没有独立 GPU。这轮 1.858s 的 DOM 结果因此被明确判为无效对照，不计入收益。
进程模式记录仍为 confirmedVulkan=true、starting=false、safeNext=false；
原生日志明确写出“GPU child failed last run; in process this once”。

追到生产代码：LaunchGpuChild 先写失败保护标记，10 秒后仅检查一次；手机
尚未成功呈现首帧时直接返回。后台启动等待 40 秒再切回，首次检查时无首帧，
此后再没有任务清除标记，虽外壳启动确认和后续播放正常，下次仍会强制回退。
这不是健康启动更快，也不是有效的 EGL 优化样本。

修复：达到十秒稳定期后仍等首帧时，保留保护标记并每秒复核；只有同一代 GPU
已呈现且未进入禁用 GL / 启动失败状态才清除。代号在新启动写标记之前更新，
写标记、换代和删除标记由同一锁串行化，防止旧回调删除新尝试的保护记录。
Pad 不等待手机 broker 的呈现状态，原有行为保留。

新增 Linux 宿主回归直接编译生产 marker/history/settling 代码块，使用手动
任务队列而非 sleep；覆盖后台四十轮后首帧清理、旧代回调、GL 回退、启动失败
和 Pad 路径。它不编译 Chromium 或 HarmonyOS 内核。仍需 CI 和连续真机启动
验收，特别是“后台启动成功 → 再冷启动”必须保持 Graphite 独立 GPU。

## 冷启动复核发现：ready 后仍滞留启动文档

第二轮受控冷启动在独立 Graphite GPU 下，ready 已回到外壳，外壳记录发出
navigate，但二十秒时 CDP 仍只有本次 data: 启动令牌页，最终 phase=frame
超时；本地恢复页服务器也未收到该轮请求。原始日志和界面失败记录保留。
这证明恢复导航未提交；尚不能只凭这些日志分清命令在哪一层合并或被后续
启动导航覆盖，不能把推测写成已定位的具体丢失点。

恢复首帧请求现在同时携带恢复网址。原生仅在当前窗口/令牌已有成功 ready、
当前文档仍是已认证启动页、没有 pending navigation 时补发恢复导航；正常
网页和已在加载的页面不会被重载。一次性 navigate 保留给旧内核兼容。
这使关键恢复动作随既有重试请求一起送达，无需重新加入诊断探针。
验收需覆盖故意省略首次 navigate 的恢复、旧内核兼容、窗口/令牌隔离，
以及正常冷启动和四十秒后台后再次启动的实际后端与持久标记。
