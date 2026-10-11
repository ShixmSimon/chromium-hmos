# HarmonyOS 功能补齐进度表

更新日期：2026-10-09。

本表记录仓库静态检查及维护者对 runner 实际配置的核对结果，作为后续逐项实现的工作清单。对比对象是 Android Chrome 的常见能力，部分能力依赖平台服务。此表不是所有 Web API 的完整审计，也不代表已经完成真机验证。

## 实现顺序与验收

以下为建议执行顺序，每次聚焦一项。状态使用：待开始、实现中、待真机验证、完成、受外部条件阻塞、不实施。只有满足验收条件后才标记完成；后续更新时记录变更位置、验证结果和剩余限制。不实施项不进入后续开发队列。

| 顺序 | 项目 | 当前差距 | 状态 | 验收目标 |
| --- | --- | --- | --- | --- |
| 0 | 渲染进程隔离（P0） | 手机仍为单进程；默认 Native 子进程共享应用沙箱。`83cd4f72` 在 Pad 模拟器 API 26 上已通过独立 UID 渲染、内置页/HTTP/HTTPS 与原生密码导入；真机 Pad/2in1、权限边界和完整回归待验证，见 [调查与实施记录](OHOS_RENDERER_ISOLATION.md) | 实现中 | 在真机复测并验证 renderer 无法直接访问 Profile 或网络、只读共享内存不能恢复写权限，覆盖跨 UID 强制终止与恢复；手机方案仍待解决，默认开关仍关闭 |
| 1 | 网页系统无障碍 | 已实现 XComponent provider 与 Chromium 网页树桥接；`build-d3aa047f` 编译通过，手机真机已验证 51 个网页节点、状态、边界与动态更新；实际读屏、平板多进程及高级语义仍待验收或补齐，详见 [接入记录](OHOS_ACCESSIBILITY.md) | 实现中 | 系统读屏能识别网页文本、链接、按钮、输入框的名称、角色和状态；支持焦点移动、点击、输入及动态内容更新；验证页面切换和多标签生命周期 |
| 2 | 跨应用粘贴 | 已增加系统安全粘贴入口及原菜单请求校验，见 [实现与验收](OHOS_SYSTEM_PASTE.md)；待编译和真机验收 | 实现中 | 不申请受限权限，验证从其他应用复制后粘贴到网页输入框及手机地址栏；本轮只做纯文本，不扩展网页 readText/外部 Ctrl+V，不返回过期内容 |
| 3 | 鸿蒙系统画中画 | 已有 Chromium 应用内小窗口，未接系统画中画；退后台后的现象尚未真机确认 | 待开始 | 视频可进入系统画中画；切换应用后按系统规则继续显示；播放控制、关闭、恢复及资源释放正常 |
| 4 | 媒体控制卡片封面 | 真机确认普通封面可显示，但空 URI 无法清除旧图；固定中性占位图修复 `ea81d58` 已通过 154 内核及引擎 HAR 增量 CI，见 [实现与验收](OHOS_MEDIA_ARTWORK.md) | 待真机验证 | 封面显示、更新及无封面替换正常；网页缓存可回收，无痕网页图片不落盘，允许固定占位文件 |
| 5 | WebOTP 短信验证码 | 按维护者决定不接入短信后端；沿用上游非 Android 分支的空 provider，见下方备注 | 不实施 | 保持无本机短信读取后端，不申请短信权限，不将此项列为待补齐功能 |
| 6 | Web Push | 按维护者决定不接入推送后端，不部署自有推送网关；已有网页通知不等于 Web Push | 不实施 | 保留现有网页通知，不将网站推送订阅及后台消息投递列入开发队列 |
| 7 | Safe Browsing | `feat/url-protection` 已实现来源选择、导航拦截、原生风险页、包内规则匹配和 HAR coordinator，见 [实现边界](OHOS_SAFE_BROWSING.md)；外壳/真实数据/官方真机及在线更新待完成 | 实现中 | 验证官方接入与开源数据，完成来源切换、风险页、更新和降级的真机验收；后续独立接入广告屏蔽 |
| 8 | Widevine DRM | runner 实际配置 enable_widevine = false，未接鸿蒙 CDM | 待开始 | 取得可用且获授权的平台 CDM 及部署路径；完成 EME、许可证获取和真机受保护内容播放验证，参见 DRM_AND_MEDIA.md |
| 9 | 通行密钥同步 | 当前 HUKS 平台认证器密钥不出设备，标记不可备份；已有密码同步不覆盖此项 | 待开始 | 先设计可同步凭据方案及信任模型；保留现有本机凭据的安全语义，验证跨设备注册、登录、恢复及撤销，不直接导出现有 HUKS 私钥 |
| 10 | WebXR / AR | 未实现对应鸿蒙平台后端 | 待开始 | 先确定 SDK、设备及支持的会话类型；验证能力查询、授权、会话启动结束和跟踪渲染；普通 WebGL 不算完成 |

## 当前 GPU 工作：硬解输出零拷贝

`OhosZeroCopyVideo` 路径通过 ConsumerSurface／NativeBuffer 直接生成 SharedImage，
避开逐帧 CPU 输出拷贝。`build-010f7882` 已通过 Mate 70 Pro+ 单进程、默认配置的
H.264、HEVC Main／Main10 SDR/PQ/HLG 零拷贝与导入复用功能验收，包括 seek、重播、
Canvas/WebGL 取帧、保留帧及 SDR/HDR 切换。HLG 1080p 完整操作场景曾有 14/123 丢帧，
后续稳定播放无丢帧，第二轮完整场景 HLG/SDR 分别丢 2/3 帧。BeginFrameDropped
不能等同有效帧超时，120/60 Hz 采样差异及逐事件原因见 [trace 分析](OHOS_HLG_TRACE_ANALYSIS.md)。
零拷贝和 SharedImage／EGLImage 导入复用默认开启，外壳无需测试开关；能力检查或初始化失败时自动回退兼容路径。
Pad 多进程、4K PQ/HLG 和 VP9/AV1 硬解尚未由本轮覆盖；尚无功耗／CPU 对照数据，详见 [实现与验收](OHOS_VIDEO_ZERO_COPY.md)。
Safe Browsing 于 2026-10-11 按维护者要求开始第一阶段开发，当前为内核/HAR 联调实现，尚未达到可宣称正式保护的真机验收标准。

## 当前 GPU 工作：动态帧率与 VSync

已接入 FrameIntervalDecider → NativeVSync 的内容帧率请求，默认开启；
纯视频按节奏请求 60–120 Hz，输入或混合动画升频，降频防抖 500 ms，闲置释放请求。
实际 BeginFrame 仍跟随系统回报周期，不把期望帧率当作实际值。
状态：待真机验证；SDR/PQ/HLG、滚动与闲置恢复的验收步骤见
[动态帧率与 VSync 策略](OHOS_ADAPTIVE_VSYNC.md)。

## WebOTP：明确不实施

维护者已决定不开发短信验证码功能。当前适配层没有替换短信 provider，沿用 Chromium 154 的 [`content/browser/sms/sms_provider.cc` 中 `SmsProvider::Create()`](https://github.com/chromium/chromium/blob/743f26418a267dd97c3c1c71d786038ae68cfc8f/content/browser/sms/sms_provider.cc#L27)：Android 创建 `SmsProviderGms`，非 Android 分支返回 `nullptr`；鸿蒙走后者。这是明确保留的空后端，不是待实现占位任务。

`content/browser/sms/sms_fetcher_impl.cc` 只有在 provider 非空时才调用本机短信 `Retrieve()`。这里的空实现只说明没有本机短信后端，不表示 WebOTP JavaScript 接口已被隐藏，也不承诺调用会立即返回“不支持”。本次仅记录产品决定和代码入口，不修改运行时行为。

## Web Push：明确不实施

维护者已决定不开发 Web Push。此前讨论的华为推送适配方案还需要标准 Web Push 网关或可接入的第三方推送服务，涉及服务端、应用凭据及持续运维。本项目不开展该方案，保持当前未接入推送后端的状态；现有网页通知能力保留。本次只记录范围决定，不修改通知、Service Worker 或网页 API 的运行时行为。

## 第一项：网页系统无障碍

### 当前进展与交接约束

`build-d3aa047f` 已完成桥接并通过 CI，手机单进程已用 `uitest dumpLayout -i`
验证网页树进入系统无障碍树。此项已有实现，后续重点是读屏验收和剩余语义补齐。
以下保留实施约束（维护者确认，2026-10-07），并同步当前状态。

- 当前内核已通过 `overlay/chrome/browser/ui/ohos/shell_accessibility_ohos.cc` 和 `overlay/components/ohos_accessibility` 接入网页树。`overlay/arkweb` 内的 ArkWeb 无障碍实现仍只作参考：当前构建 `enable_arkweb = false`，该部分未编译。
- 网页绘制在外壳的 `XComponent` 上；内核侧使用 Aura 窗口及 `overlay/ui/ozone/platform/ohos`。接入方向是通过鸿蒙 NDK 的 `OH_ArkUI_AccessibilityProvider` 为该 XComponent 提供节点，将 Chromium 的 `ui::AXPlatformNode` / `BrowserAccessibilityManager` 数据和操作映射到系统接口。实施时核实实际 SDK 的接口与生命周期要求。
- 手机使用 `--single-process`，平板和 PC 使用多进程。两种进程模式都必须验证，不能只在一种模式通过后标记完成。
- 改动遵循增量补丁流程，在 `scripts/ci-incremental-build.sh` 登记。同一文件的补丁不能有相邻的修改段；组织补丁时合并相邻修改，检查应用顺序与上下文。
- 在 CI 上编译，不在本地编译。
- 在真机上验证；操作设备前先检查当前前台是否正在使用其他应用，避免打断使用。若其他应用正在使用，先协调测试时机，不直接切换或强停。
- 不把网页文本或原始网址写入 hilog。确需记录网址时使用 `UrlForLog()`，并核实该辅助函数的适用范围；无障碍节点调试日志同样遵守此约束。

下一步基于已有 Aura/XComponent 链路验收和补齐：

1. 在手机真机开启系统读屏，验证首次查询、触摸探索、焦点高亮、点击、编辑与滚动。
2. 在平板多进程及隔离 renderer 下验证网页树、跨站 iframe 和操作回传。
3. 覆盖多标签、导航、renderer 退出、窗口偏移/缩放及 Surface 重建，确认没有失效节点和坐标错误。
4. 按 [接入记录](OHOS_ACCESSIBILITY.md) 补齐高级角色语义、文本选择、aria-live 和读屏关闭后的模式释放。

## 已核实，不列为缺失

- JIT：维护者核对 runner 的 out/ohos_arm64/args.gn 后确认 v8_jitless = false，Sparkplug、Maglev、TurboFan 开启。启动时按配置和可执行内存能力回退，本次状态查看 aboutInfo.jitEnabled。仓库示例配置已同步相关 JIT 开关，但不代表整个文件与 runner 一致。
- WebAssembly：已有支持，执行模式需结合实际构建及启动配置判断。
- 普通音视频及硬件编解码：已有实现，与 Widevine DRM 分开验收。
- 网页通知、媒体会话：已有实现，分别不等于 Web Push 和封面已完成。
- 本机 WebAuthn、系统身份验证、密码同步：已有实现，不等于通行密钥同步。
- 定位、摄像头、麦克风、屏幕共享、传感器、振动、电池、屏幕常亮：已有接入。
- 蓝牙 BLE、USB/HID/串口：已有后端或桥接，具体设备及协议覆盖仍需单独验证。
- 语音合成、形状检测、方向锁定、文件选择、分享、打印：已有实现或桥接，部分功能依赖外壳响应。

“已有实现”仅表示存在代码或接入记录，不代表所有设备和场景测试通过。

## 进度记录

- 2026-10-08：校正无障碍过期状态：已有实现且 `build-d3aa047f` 编译、手机真机树导出验证通过；实际读屏及多进程验收仍待完成。
- 2026-10-07：开始网页无障碍实现，增加 provider、树快照、操作回传、增量构建补丁和真机测试页。仍需 CI 编译与手机/平板真机验收，未标记完成。
- 2026-10-07：建立清单，记录维护者核对结果；尚未开始本表功能的实现和真机验收。第一项安排网页系统无障碍。
- 2026-10-07：补充维护者交接约束：XComponent NDK provider 接入方向、单/多进程覆盖、增量补丁登记、仅 CI 编译、真机前台检查及日志隐私要求。
