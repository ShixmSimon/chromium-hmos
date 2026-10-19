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

手机已安装包实际使用新的 startup handshake；当前 Lumie 主分支仍是旧的
PageTextRequests / StartupHealth 实现，不能据本地主分支猜测设备的等待路径。
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
