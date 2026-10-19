# GL / Skia Vulkan 自动亮度与 10 位输出测试

应用默认进程模式不变。GL 自动读取系统亮度，无需增加配置；手机版独立 Skia
Vulkan 继续使用已有实验开关：

```ts
config.experimentalPhoneFork = true;
config.gpuProcess = 'separate';
config.skiaBackend = 'vulkan';
config.angleBackend = 'gles';
// vulkanOutputMode 默认 auto，无需填写两个亮度参数。
```

外壳使用新 HAR 中的 WebWindow / NativeContext，即自动接入
`display.getBrightnessInfo()` 和 `brightnessInfoChange`（API 22）。直接调用
C++ NAPI 而绕过 NativeContext 的外壳，需要调用 `UpdateDisplayBrightness`，
参数依次是 displayId、sdrNits、currentHeadroom、maxHeadroom，并转发变化事件。

`auto` 默认尝试 10 位：屏幕支持 HDR10 且取得亮度时使用 HDR10/PQ；否则尝试
SDR10。实际输出目标失败会记录原因并回退 SDR8。透明独立窗口仍为 SDR8。
10 位描述缓冲区精度，Skia Vulkan 名称本身不能证明实际输出已是 10 位。

## 怎么测

安装新 HAR 打出的应用，冷启动后正常浏览、播放已有 HDR 内容，并调节系统亮度，
观察白底、暗部、高光及前后台切换。无需手填 203/1000，也无需逐组切换参数。
如果画面异常，保存当次日志后使用以下配置并完整重启：

```ts
config.vulkanOutputMode = 'sdr8';
```

该开关只关闭 Vulkan 的 10 位/HDR 输出。若 Vulkan 初始化本身仍崩溃，可把
`skiaBackend` 改回 `'gl'` 后重启；独立 GPU / renderer 配置不必改成单进程。

## 日志判断

- `OHOS brightness: display=... white_nits=... peak_nits=... max_headroom=...`：
  已取得系统亮度。由系统事件驱动更新，不逐帧查询系统服务。
- `OHOS Vulkan: creating instance; requested_layers=0 swapchain_colorspace=...`：
  Skia Vulkan 正在初始化。后面的 instance initialized 才表示创建成功。
- `OHOSVulkan` 标签下的 `Dawn vkCreateInstance begin`、`requested layer`、
  `available layer`、`succeeded` 区分 WebGPU 创建阶段；可用于定位 layer 加载崩溃。
- 手机 `OHOS phone Vulkan: negotiated output mode=... native_format=...`，以及
  `OHOS phone Vulkan: active mode=...`：先协商，再确认实际提交。
  mode 0/1/2 分别表示 SDR8/SDR10/HDR10；native_format 12/34 表示 RGBA8/RGB10A2。
- `HDR luminance updated white_millinits=... peak_millinits=...`：Vulkan 已收到
  与该帧色彩转换一致的更新。这里单位是毫尼特；除以 1000 得到 nits。
- GL 保留原有 reshape / 色彩空间日志，亮度更新沿 DisplayColorSpaces 生效。
- 平板/PC WSI 日志包含实际 swapchain format、color_space、output_mode。

外壳的 renderer JS 回应、配置开关为 true、屏幕支持 HDR，都不能单独证明 Vulkan
已经工作。普通截图可以辅助发现偏色，通常经过 SDR 转换，不用于证明面板位深。

## 已知边界

Vulkan 输出模式在窗口初始化时选择，调亮度不会切换 SDR/HDR 格式或重启 GPU。
动态参数使用系统当前 headroom，最大 headroom 只作为上限；headroom=1 是合法的
临时状态。可选 `vulkanHdrWhiteNits` / `vulkanHdrPeakNits` 仅用于固定值调试，
正常使用时应删除，避免覆盖自动更新。

目前 Vulkan HDR 限单显示器；外接屏变化可能触发 SDR 回退。Vulkan 每个输出单独
协商色彩，浏览器全局 CSS HDR 声明仍保守。视频 Vulkan 零拷贝留到此轮输出完成后。

68e8b50 真机报告已确认发生 Vulkan layer 加载阶段 SIGBUS，不能据此判断面板不支持
10 位。本轮修正 Skia 的扩展误传 layer 参数，并保留 fork 后链接器需要的 RELRO 缓存 FD。
现存完整 crash 确认 Dawn 创建 instance 时加载 Trace3D，RELRO 写入目标却映射了
Chromium 共享内存，详见 [诊断记录](OHOS_VULKAN_RELRO_FD_DIAGNOSIS.md)。
新包先看 instance 成功及实际提交日志；修复是否消除 SIGBUS，以实测为准。
如果再次崩溃，请同时保存完整 native crash（含 maps / OpenFiles），避免只有 hilog 摘录。

## 320ca09 后的回退修正与 FD 诊断

这版 SDR8/SDR10 只设置 sRGB，不再向 NativeBuffer 发送无法表示的 `OH_VIDEO_NONE=-1`。
HDR 队列转 SDR 或 GPU 重启时，先退出旧 lease，再通过 `OH_NativeWindow_CleanCache`
回收旧 HDR 缓冲区。正常连续绘制、HDR 亮度变化不会触发逐帧清理。

1ef4721 跟踪已定位到 fstat EACCES 导致误关缓存，本轮修正该保留条件，真机待验收。
保持原来的 separate GPU + Skia Vulkan
配置，换 HAR 后正常冷启动一次即可，不需要另加实验参数或重复切换模式。

- 外壳 `config.userDataDir` 目录自动生成 `PhoneForkFdTrace.log`；启动日志会显示
  `phone FD trace enabled=1`。如果为 0，先保留启动日志，确认目录可写。
- 保存同一次运行的该文件、hilog，以及发生崩溃时的完整 native crash。
  文件每次应用冷启动重写，约 8 MiB 上限，所以收集后再启动下一轮。
- 日志按 PID / stage 对比同一 FD 的 target、dev、inode、size、flags 和 errno。
  `name_match` / `keep_relro` 表明当前保留规则是否命中。
- 关键 stage：`browser-before-server-fork`、`server-after-fork`、
  `server-before-cleanup`、`server-after-cleanup`、`server-before-child-fork`、
  `forked`、`loading`、`entry`、`native-main`、`descriptors`、`chrome-main`、
  `skia-before-loader`、`skia-before-vkCreateInstance`、`dawn-before-loader`、
  `dawn-before-vkCreateInstance`。成功创建后还有对应 after 标记。

记录固定缓冲区及文件 write，不在早期 fork 子进程初始化 hilog，不记录网页 URL/启动参数。
它会包含应用打开文件的路径；作为本地诊断证据保存。只在手机实验 fork 模式启用，
单进程及官方 NCP 不创建该文件。

## 1ef4721 后的复测重点

正常配置冷启动一次，保存同次日志即可，不必逐项切换参数：

1. `PhoneForkFdTrace.log` 中原 RELRO FD 在清理后、Skia/Dawn 创建 instance 前仍是
   `relro_cache`，`keep_relro=1`。`stat_errno=13` 可以仍然存在，不表示本次保留失败；
   看原编号是否保留，而非固定要求编号为 19。
2. hilog 搜索 `OHOS phone output: color probe requested`，先确认请求的 mode 1/2，
   再看 `color probe guards`。mode 1 是 SDR10，mode 2 才是 HDR10。
3. 如果继续回退，保存第一条 `OHOS shared buffer: step=...` 或
   `OHOS buffer color: step=...` 及相邻日志。`native_error` 是对应 API 的原始返回值；
   `parcel-schema` 会列出请求、实际分配和序列化的尺寸/格式；
   `color-space-readback` 会列出预期、实际颜色空间和返回值。
4. instance 成功后仍需 `active mode` / 首帧确认实际后端及位深。

本轮 FD 修复仅影响手机实验 fork；新分配诊断只在失败时记录，probe 日志仅在协商时
记录，不给成功绘制增加逐帧日志。若新包仍 SIGBUS，继续保存完整 native crash。


## ce4ac7d 后的等价色彩空间修正

ce4ac7d 真机请求的是 SDR10（mode=1）。NativeBuffer 设置和读取均成功，系统把
`SRGB_FULL=11` 读回为 `DISPLAY_SRGB=25`；SDK 明确规定二者等价，但旧的数值比较
使 SDR10 和后续 SDR8 都被拒绝。这不是系统 API 失败，也不是面板位深不支持。

本轮先归一化 SDK 明确规定的输出别名再比较：sRGB 的 11/25，以及 HDR10 的
`BT2020_PQ_FULL=5` / `DISPLAY_BT2020_PQ=31`。非零 API 返回、不同色域、
传递函数和范围仍失败。成功路径不增加逐帧日志。

替换新 HAR 后，保持原 separate GPU + Skia Vulkan 配置，正常冷启动一次即可。
此轮重点检查 `negotiated output mode=1 native_format=34` 与后续 `active mode=1`
及 Vulkan 首帧；若系统自动选择 mode=2，则按 HDR10 实际格式与首帧验证。
不再出现 expected=11 / actual=25 的失败，单独这一点还不足以证明最终出帧成功。
出现新的失败时保存首条 step 日志及相邻上下文；无需重复切换或连续重启旧包。

ce4ac7d 的 FD 跟踪显示原缓存保留且 Dawn 到达 after-vkCreateInstance，未发现新
SIGBUS。证据包里的 native-crash.log 是 1ef4721 / 12:48:26 的历史文件，不能算新崩溃。
# 2026-10-14：首次亮度与 HDR 选择诊断

`ed3aece` 现场继续运行 Vulkan + SDR10，所采集日志内未再出现 SharedImage 创建失败和
不存在 mailbox 的错误。这不等同于 HDR10 通过，也不替代 Canvas / WebGL 画面验收。

14:50:37 首次出现有效亮度 hilog，不能证明亮度比启动晚到 21 秒：
`DisplayBrightnessBridge.start()` 在 `Initialize()` 前同步查询；首次 NAPI 更新和
启动参数快照发生在 `RunChromeMainOnThread()` 安装 Chromium hilog handler 之前。
此外，SDR10 协议包要求 white / peak 为 0；即使启动亮度有效，`auto` 模式的显示器
HDR 能力检查失败也会选择这个包。现有证据不足以区分这两种原因。

本轮仅补诊断，不加入启动等待、重试、固定亮度或动态 SDR→HDR 格式切换。
沿用 `vulkanOutputMode='auto'` 和系统亮度，不设置手工 nits，冷启动一次并保留以下日志：

| 日志标识 | 能回答的问题 |
| --- | --- |
| `OHOS brightness bridge: source=first-query` | 同步查询实际返回的三个亮度字段 |
| `OHOS brightness NAPI:` | 首次接收成功，或参数类型 / ID / 数值校验失败 |
| `OHOS HDR startup:` | 生成启动参数时是否有有效亮度，以及最终传出的数值 |
| `OHOS output color selection:` | GPU 收到的启动数值、选择的模式和原因 |
| `OHOS HDR display probe:` | 能力查询返回码、显示器数量、ID、状态和 HDR 格式列表 |
| `OHOS brightness bridge: source=first-event` | 第一次系统事件与同步查询的值是否不同 |

`reason=startup-brightness-unavailable-or-invalid` 再回查查询 / NAPI / 启动快照；
`reason=display-capability-gate` 则检查显示器枚举与 HDR10 格式，不再归因于亮度时序。
`reason=hdr10-request` 后仍未上 HDR10，应检查 buffer probe / display guard / 回退日志。
最终成功仍以 `active mode=2`、实际 HDR10 色彩空间及 metadata 为准。

诊断仅记录首次查询、首次事件、各 NAPI 结果的首次出现，以及输出创建时的选择；
不在每帧增加查询或日志。显示守卫原有周期检查也不打印新诊断。

## 2026-10-14：手机 fork GPU 的显示能力查询改由主进程代办

`fea3674` 真机诊断确认首次查询、NAPI、启动快照和 GPU 参数均收到 284.736 nits。
实际失败的是 GPU 内 `OH_NativeDisplayManager_CreateAllDisplays`，返回 1400003，
因此选择 SDR10。这个系统异常码不代表显示器不支持 HDR10；更底层的服务代理状态
或访问限制尚未区分。

修复只对 `IsOhosPhoneGpuChild()` 生效：启动选择和 `OhosHdrDisplayGuard` 共用的
能力查询经现有私有通道交给浏览器主进程。主进程仍调用官方显示 API，要求单显示器
且支持 HDR10，再返回支持状态和完整 display ID。守卫保留一秒检查周期、显示器 ID
一致性检查和异常时现有的安全回退；不缓存一个永远有效的启动标志。
普通 Native Child Process、平板 / PC 和单进程仍保留原有查询方式。

协议新增无 FD 的显示查询 / 响应，版本升至 PGS4；旧版本、携带 FD 的控制请求和
错误消息类型不能当作能力通过。主进程的颜色探测仍独立验证 HDR 能力、NativeBuffer
格式与 metadata，只有真实协商成功才发布 HDR10 输出能力。没有额外逐帧显示服务查询。

复测保留系统亮度和 `vulkanOutputMode='auto'`，替换 HAR 后完整冷启动：

1. 主进程日志 `OHOS HDR display probe: result=... count=...` 及 HDR 格式列表应给出
   实际查询结果；GPU 日志应为 `via=browser supported=1 id=...`。
2. 检查 `reason=hdr10-request`、`negotiated output mode=2` 和首帧 `active mode=2`。
   仅转发查询成功不算 HDR10 已启用。如果主进程也失败，保留其返回码和格式列表。
3. 保持播放 / 滚动超过数秒，确认周期检查后仍维持 HDR10，没有 GPU 退出、SDR8/GL
   回退或纹理错误。观察亮度、黑位、颜色及前后台恢复。

本地协议测试覆盖 ID=0、64 位 ID、支持 / 拒绝 / 错误返回、旧版本拒绝及控制消息
FD 清理。目标编译由 Chromium 154 CI 验证；主进程在真机的能力查询和 HDR 上屏仍待复测。

## 2026-10-14：HDR 输出普通网页发灰的元数据修正实验

9626b70 已在 Mate 70 Pro+ 实际输出 `active mode=2`，但这仅证明 HDR10 启动、
出帧成功，不代表色彩和亮度验收通过。现场普通网页发灰，系统截图和 Chromium
截图的灰阶不同：源值 32 / 64 / 128 / 192，在 Chromium 截图中保持原值，系统
截图约为 45 / 70 / 118 / 165。截图不能测量面板实际 nits，也不能单凭此结果
证明最终 NativeBuffer 像素正确。RenderService 消费端确认收到 BT2020/PQ 标签
和 maxCLL / maxFALL = 1600 的 metadata，未发现标签在传输中丢失。

本轮修正一个可确定的语义问题：手机 HDR 输出也承载 SDR 普通网页，旧代码却
一律把显示器可用 HDR 峰值当作当前帧内容峰值。现在根据根 render pass 的
`content_color_usage` 生成帧亮度范围：SDR/WCG 帧的峰值上限为当前系统 SDR
白点，HDR 帧保留系统可用峰值。显示器能力快照不被修改，后续 HDR 帧仍能恢复
完整范围。白点仍来自系统，不固定 203 / 1000 nits；不改变 PQ、10 位格式、
NativeBuffer 传输或协议版本，仅在手机 fork 独立 Vulkan GPU 中启用。
没有加入逐帧日志、显示服务调用、CPU 像素复制或新的等待。

该修正对发灰的改善效果尚待真机对照，不能预先认定为根因。验证步骤：

1. 保持同一系统亮度设置、页面及启动配置，更新 HAR 并完整冷启动；确认新内核
   标识、HDR10 首帧和没有 GL/SDR 回退。
2. 普通页面检查有效帧 metadata 的 peak 与 white 相等；渲染服务消费端应收到
   对应峰值。初始 probe 的峰值仍可为显示器峰值，它不用于绘制实际页面。
3. 对照 0 / 32 / 64 / 128 / 192 / 255 六级中性灰；截图包含所有色块，分别保存
   Chromium 截图和系统截图。比较灰阶曲线、网页白色与原生外壳白色，观察真机。
4. 播放确认为 HDR 的内容，检查峰值恢复可用 HDR 范围；退出回普通页面后恢复
   SDR 白点上限，并检查 Canvas / WebGL、滚动及前后台切换。

本地回归覆盖 SDR→HDR→SDR、系统 headroom 改变、SDR8/SDR10 不变，以及现有
共享缓冲区传输 / FD / fence / 颜色回退。旧版偶发启动黑屏尚未定位，不因本次
元数据实验而视为修复。

### 真机结果及后续修正：按内容选择 SDR10 / HDR10

82530b8 已编译、签名安装并确认 HDR10 首帧成功。RenderService 消费端确实收到
修正后的峰值（376.799 nits），但普通网页依旧发灰。去掉弹窗遮罩后，灰阶
0 / 32 / 64 / 128 / 192 / 255 显示约为 0 / 45 / 70 / 118 / 166 / 204。
前后系统动态白点不同，不能把数值差异称为元数据修正的改善效果。
因此撤去该实验的峰值钳制及专用 capability，不保留一个未证实有效的特例。

后续改为按 Chromium 已有 content usage 选择输出：仅对已成功探测 HDR10 的
手机 Vulkan 输出，将 kSRGB 项设为 sRGB + RGBA_1010102；HDR 项仍为 BT2020/PQ
10 位，系统动态白点和 headroom 继续更新。WCG 仍走原 BT2020/PQ 路径，以免
在尚未接入 P3 NativeBuffer 输出前静默裁剪为 sRGB。WCG/HDR 场景的亮度仍须
独立验收，本次先解决普通 sRGB 网页不应被统一按 HDR 视频处理的问题。

SDR10↔HDR10 切换仅在已经验证 HDR 能力的输出中允许。切换发生于无活跃 lease
的 Reshape：先退休 Vulkan imports，再执行现有 color probe，取得新 generation，
取消未绘制的 probe buffer，然后以新颜色创建后续绘制表面。浏览器已有 HDR→SDR
清队列策略防止 HDR metadata 残留，旧 generation 仍被拒绝。正常帧不增加等待、
probe 或日志；模式切换允许一次同步退休。此变化不新增协议字段，不影响普通
NCP / WSI、单进程 GL 或固定 SDR10 输出。

真机重点：普通网页首帧 active mode=1、灰阶恢复；HDR/WCG 内容进入 active mode=2，
退出后回到 mode=1 且无 stale-generation、GPU 重启、HDR metadata 残留。SDR10
是按内容选择的 10 位模式，不是 GPU 故障回退。切换日志和首帧日志应配对。

### 后续修正：宽色域 SDR 不再触发 HDR10

2024b407 真机反馈最低系统亮度时普通网页显著偏暗，前台恢复时缓慢变亮。
同期日志有 `active mode=2`、`white_millinits=2000` 及连续变化的峰值亮度；
NodeSeek 的 CDP 截图仍为浅色。尚未证实该页面具体由哪个内容触发 HDR。

修正手机 fork Vulkan 的输出映射：`kSRGB` 和 `kWideColorGamut` 两项均使用
sRGB + RGBA_1010102，只有 `kHDR` 保留 BT2020/PQ。宽色域不等于高动态范围，
不应仅因 P3 图片、CSS 或 Canvas 就将整页交给 HDR 色调映射。P3 SDR 的
NativeBuffer 输出尚未接入，本次明确选择将 WCG 转换到 sRGB，无法保留超出
sRGB 的色域；不能将此称为 P3 输出支持。HDR 亮度参数、能力探测和队列切换
机制保持原有行为。

用 `docs/test-pages/sdr-wide-gamut.html` 做输出回归：

1. 在最低系统亮度打开，记录 sRGB 灰阶、白色背景及 `active mode=1`。
2. 显示 P3 CSS / Canvas 色块，确认 Canvas 实际为 `display-p3`；灰阶不应变暗，
   输出应维持 mode=1。再隐藏色块并进行前后台切换，白色不应渐变。
3. 播放已确认的 HDR 素材，检查 mode=2；返回此页检查 mode=1、灰阶及无残留
   HDR metadata。HDR 本身的低亮度映射仍需独立验收。

修改后的编译和真机结果应单独记录，以上步骤不代表已经通过真机验证。

## 2026-10-14 夜间真机调试：SDR 恢复、HDR 能力与视频上传

设备 PLA-AL10 / HarmonyOS 7.0.0.109 / Maleoon 920；Lumie 手机 fork 独立
GPU，Skia Vulkan，ANGLE GLES。保持用户最低系统亮度，系统报告 SDR 白点
2 nits。调试期间用 `power-shell timeout -o 3600000` 延长自动息屏时间并定期
续期，未通过锁屏切换来测试后台恢复。以下结果取代上文对应的待验证实验结论。

### 已定位并修正

- `86d3e3c9`：普通 sRGB 和 P3/WCG SDR 页面均输出 SDR10 / mode=1；只有
  HDR 内容进入 PQ / mode=2。P3 SDR 暂时转换到 sRGB，不能称为原生 P3 输出。
- `aee8cf89`：硬解 P010 CPU 帧原先触发 VideoResourceUpdater 不支持格式的
  NOTREACHED，导致 renderer SIGTRAP。改用 libyuv 无损转为 I010，保留
  10 位、BT2020/PQ 与 HDR metadata。源平面大小、stride 和对齐仍检查。
- 同提交修正全屏切换的 NativeBuffer 分配检查：实际出现 1617×175、
  stride=6656、allocation=1163264 的有效 optimal-tiled buffer，不能要求
  allocation ≥ stride×height。保留逻辑像素大小下限、分配上限、尺寸、stride、
  FD、lease 和 generation 检查；Vulkan 导入也检查实际分配上限。
- `ef41993c`：物理面板白点不等于 PQ 内容参考白。内容白点归一化到
  203 nits，保留系统 peak/white 比例及物理亮度诊断。避免把最低亮度的
  2 nits 先编码进内容，再被系统映射一次。OpenHarmony RenderService 的
  `RSBaseHdrUtil::DEFAULT_SDR_NITS` / HDR 映射使用 203-nit 内容参考白：
  <https://github.com/openharmony/graphic_graphic_2d/blob/master/rosen/modules/render_service/core/feature/hdr/rs_hdr_util.cpp>。
- 同提交加入经过 GPU 实际导入探测后发布的 HDR 能力。原先普通 SDR 页面
  报告不支持 HDR，YouTube 因此只提供 SDR 菜单，即使源包含 HDR。现在探测
  成功后发布能力，在颜色失败、Vulkan 失败及 GPU 连接关闭时撤销。
- `2496eac1`：OHOS Vulkan CPU 上传优先使用 HOST_CACHED 内存，保留
  Skia/VMA 非 coherent 内存 flush。设备实际选到 memory type=1、flags=11。
- `4c4d755e`：phone Vulkan 根据 Skia 的 A16_UNORM 实际支持情况启用 R16
  视频平面，避免不必要的 10-bit→FP16 转换；设备日志确认支持。
- `9418fe70`：浏览器媒体服务直接把硬解帧写入只读 IPC 共享内存的本地
  writable mapping，避免 Mojo 序列化时再次分配、复制完整视频帧。每帧独立
  持有 region/mapping，不在远端可能仍持有时复用；完成写入后才发布 SHMEM。

### 已执行的正确性检查

- 灰阶测试页使用 `color-scheme: only light` 隔离手机强制深色策略。sRGB、
  P3 CSS、实际 display-p3 Canvas、三次前后台切换均维持 SDR10，白色控制块
  恢复到系统截图 255。原始 NodeSeek 页面重新加载后为可读深色主题；不将
  强制深色网页本身作为中性灰阶对照。
- HEVC Main10 HDR10 `bear-1280x720-hevc-10bit-hdr10.mp4` 为约 29.97 fps。
  真机首帧 `VideoFrame.copyTo()` 得到 I420P10 / BT2020/PQ，2764800 bytes；
  与 FFmpeg 解码首帧 SHA-256 完全一致：
  `80360cd8644e9fc83c02bb3f87dae58e1bf7bca82f878bec7bd071fa52825580`。
  样本覆盖低两位 0、1、2、3，未降为 8 位。
- YouTube `diLzHHuJeuE` 菜单现在显示 HDR，实际选到 itag 335/336，
  1080p60/1440p60、BT2020/PQ，原生输出 mode=2；退出 HDR 后回到 mode=1。
  不能仅凭播放菜单或 capability 宣告实际 HDR 已输出。
- Canvas 2D、WebGL2 清屏读回 RGBA `[64,128,191,255]`；WebGL 视频纹理
  上传 / FBO 读回无 GL 错误，framebuffer complete，context 未丢失。
- 多次 HDR/SDR 和全屏切换后，4c4d755e 同一 GPU 进程保持运行，未再看到
  packet validation 失败、SIGTRAP、GPU 崩溃或 SDR8/GL 故障回退。
- HDR 系统截图经过系统映射，不能据此测量面板 nits。本次没有光度计，
  不宣称 HDR 的物理亮度或色准已完成仪器验收。

### 性能结果与未通过项

修复前 1080p HDR 的 GPU `WritePixelsYUVINTERNAL` trace 平均 12.82 ms；
缓存上传后一次对应采样为 2.54 ms。两次 trace 的取样段不同，仅证明上传
耗时明显下降，不能作为整机综合基准。

4c4d755e 对 YouTube 1080p60 同一 32.65–42.66 s 片段，10 s 内
599 帧丢 27 帧（4.5%）；较 ef41993c 同片段 600 帧丢 264 帧改善。
1440p60 全屏约 32.65–42.27 s，594 帧丢 199 帧（33.5%）。
720p 的重复采样也有明显波动，不能标成稳定流畅。

设备 AVCodec 能力查询无 VP9/AV1 硬解，YouTube VP9 使用软件解码；但
本地 1440p60 HEVC Main10 硬解同样出现掉帧和播放停顿，不能把问题全归于
YouTube 编码。对硬解 trace，Mojo `VideoFrameDataPtr::CreateRegion` 平均
10.58 ms/帧，进一步促成 9418fe70 的共享内存改动。

因此本轮不能宣告“手机 Skia Vulkan 全部正常”或“全部测试通过”。
已执行的协议、颜色策略、亮度、fork、补丁回归与 public-tree 检查通过，
但并未运行 Chromium 全量测试套件，高分辨率视频流畅性仍须独立验收。

### 9418fe70 安装后的复测

HAR 与签名 HAP 内的 `libweb_engine.so` SHA-256 一致：
`0af7e48584ce525d3a3927dd399846488b2d9e4921432b2b6809ac1c1b6b0f02`。
执行 clean 后重打包再安装，避免应用增量打包误用旧 HAR。

- HDR 首帧仍与上述 FFmpeg SHA-256 一致；保留首帧并继续播放 5 s，
  再次复制该帧得到相同哈希，未被后续帧覆盖。
- H264 SDR 对照为 NV12，首帧 SHA-256
  `8e42304d3920c90825839f1deac12cf48e05daffef09ad9d29fc116ee442e8c5`
  与 FFmpeg 一致；旧帧保留测试也通过。
- Canvas/WebGL RGBA 精确读回、HDR 视频 WebGL 纹理上传再次通过。
- 同一 1440p60 HEVC 素材的共享内存封装 trace 均值由 10.58 ms 降到
  0.0127 ms；但 `OhosVideoDecoder::CopyOutput` 仍约 17.94 ms，
  `CreateForSoftwareFrame` 约 8.25 ms，GPU 上传约 3.87 ms。各项跨线程
  可能重叠，不能简单相加当作端到端延迟；也未排除调度/设备状态波动。
- 本地 HEVC 1440p60、10 s 墙钟采样：565 帧丢 261 帧，媒体时间前进
  约 9.36 s。消除重复 IPC 复制不等于解决硬解路径的全部性能问题。
- YouTube 固定 1080p60 片段：601 帧丢 27 帧（4.49%）；1440p60 全屏
  同片段：600 帧丢 16 帧（2.67%）。后者比此前结果好得多，但该共享内存
  修改不作用于 VP9 软件解码，不能把变化归因于它，也不能据单次结果宣称
  稳定达标。连续一分钟结果单独记录。

连续一分钟 YouTube 1440p60 全屏结果：3597 帧丢 121 帧（3.36%）。
各 10 s 窗口丢帧为 8 / 11 / 15 / 10 / 55 / 22，始终为可见页面、
2560×1440、全屏，媒体时间正常前进。中间仍有一段约 9.2% 的掉帧；
未达到稳定无明显掉帧的验收目标。原始计数、帧哈希、包哈希和检查范围见
[`test-results/ohos-phone-hdr-2026-10-14.json`](test-results/ohos-phone-hdr-2026-10-14.json)。

9418fe70 最后又完成三次 Home→恢复，宽色域 SDR 测试页仍为 mode=1，
白色背景、六级灰阶正常，无 filter/opacity 遮暗；同一 GPU 31585 在
23:48–23:57 的捕获日志中未重启，未发现 SIGTRAP、包验证失败或 GL/SDR8
故障回退。手机保持亮屏并停留在该测试页，测试视频已暂停。


## 浏览器解码 Surface → 手机 Skia Vulkan（开发中）

`a358dbea` 引入了浏览器 AVCodecKit Surface 缓冲区的跨进程导入，尚未完成
真机验收，不能把前文 `9418fe70` 的通过记录算作此实现的测试结果。

- 浏览器保持解码器及 Surface 的所有权；GPU 只导入 NV12 / P010 原生缓冲区。
- 通过 `NativePixmapHandle` 传送有界的 NativeBuffer parcel 和文件描述符。
  parcel 使用系统内部布局，未知布局拒绝导入，不把内存当成线性像素读写。
- 生产者 fence 通过 SharedImage 更新传入 Vulkan，P010 位深及 PQ / BT.2020
  元数据继续保留；没有 GLES 使用权的外部采样纹理由 Raster 转换供 WebGL 使用。
- 每帧独立的 socket 端点控制解码队列租约，最后一个 GPU 导入对象在 GPU
  完成后销毁，浏览器才归还原生缓冲区。暂停、丢帧后也主动检查完成 fence。
- 没有导入能力、功能关闭或 Surface 初始化失败时，初始化阶段仍使用原来的
  CPU 共享内存路径。播放中导入失败的恢复行为尚待真机验证。
- 导入代码使用 Khronos `vulkan_ohos.h` 的结构定义；相关契约见
  [NativeBuffer 内存导入](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportNativeBufferInfoOHOS.html)
  和 [OHOS 外部格式](https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalFormatOHOS.html)。

已完成：新增解码、导入、共享图像和浏览器服务目标的定向编译；主机端 parcel
边界、FD 最后持有者释放、手机 GPU 通道回归测试；补丁重复应用和公开树检查。
完整内核构建及安装、HDR / SDR 像素对照、WebGL 视频纹理、暂停 / 跳转 /
前后台恢复、长时间掉帧测量尚未完成。

### 非 fork Skia Vulkan 原生视频导入（待 CI 编译及实机验收）

非 fork 的 GPU 进程内解码器现接入同一 NV12 / P010 Vulkan 导入函数。
该路径直接传递 `NativePixmap` 引用，不序列化 NativeBuffer parcel，也不建立
释放通知 socket。Skia 通过只读 SharedImage 采样原始解码缓冲区，保留源色彩
空间、HDR 元数据和 P010 位深。

- 非 fork Vulkan 实现将 `VK_OHOS_external_memory` 列为可选扩展；原生视频
  导入还要求 external-semaphore-fd、foreign queue family 和属性查询函数。
  能力不足或 Surface 初始化失败时使用原有缓冲输出；加密视频不走此 Vulkan
  路径。GL/EGL 继续使用原有导入及缓存逻辑。
- 解码器的 acquire fence 导入 Vulkan 信号量；每个 VkImage 持有原始 Surface
  队列租约，GPU fence 完成后的销毁才归还缓冲区。Vulkan 暂不缓存逐帧导入，
  避免缓存占住有限的解码队列。暂停、丢帧后的释放使用同一 cleanup fence 轮询。
- Vulkan 视频 SharedImage 仅声明 DISPLAY_READ / RASTER_READ；WebGL 视频
  纹理经 Raster 转换。此改动没有补齐原生 Display-P3 SDR 输出。

验收须分别记录 fork 与非 fork、GL 与 Vulkan 的真实后端，并固定同一份
HEVC Main10 素材及清晰度。覆盖首帧与保留帧正确性、持续播放掉帧、全屏、
暂停与跳转、后台恢复、切换视频、GPU 重建及内存/FD 占用；另外复测 H264 SDR、
VP9 软件解码与 WebGL 视频纹理。旧版本或手机 fork 的结果不能代替非 fork
设备验收。构建仅通过推送触发的 GitHub Actions 执行。
