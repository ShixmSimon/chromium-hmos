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
