# Skia Vulkan 10 位 / HDR 外壳测试

此轮实现输出目标与色彩合成。真机结果待验证，不能把编译通过、API 成功或
`chrome://gpu` 的 Vulkan 标识当成实屏 HDR 通过。视频 Vulkan 零拷贝另开一轮。

## 外壳启动配置

使用本次构建的 engine HAR，替换依赖后清理外壳构建产物，完整退出应用再启动。
所有选项在 `ensureStarted` / 启动配置中传入，运行中修改会返回失败，需要冷重启。
默认仍是单进程和 SDR8；下面是手机实验配置：

```ts
config.experimentalPhoneFork = true;
config.gpuProcess = 'separate';
config.skiaBackend = 'vulkan';
config.angleBackend = 'gles';
config.vulkanOutputMode = 'sdr10';
```

先比较 `sdr8` 与 `sdr10`，通过后再尝试：

```ts
config.vulkanOutputMode = 'hdr10';
config.vulkanHdrWhiteNits = 203;
config.vulkanHdrPeakNits = 1000;
```

203/1000 只是可替换的实验参数示例，**不是 Mate 70 Pro+ 的实测数据**。
参考白范围 80–500 nits，峰值大于参考白且不超过 10000 nits；需要按设备与测试
条件校准。HDR10 缺少有效参数会回到 SDR8。不要用视频 MaxCLL 代替面板峰值。
`auto` 有有效参数且显示器支持 HDR10 时尝试 HDR10，否则尝试 SDR10；实际分配或
WSI 不支持时仍可能回到 SDR8。首轮用显式模式，便于对照。

平板/PC 保留官方子进程配置，使用相同色彩选项；不需要手机 fork 开关。
其 HDR 路径另需真实 10 位/PQ swapchain 格式对及 `VK_EXT_hdr_metadata`，
不支持时保持 SDR。手机成功不能代替平板/PC 验收。

## 如何确认真的启用了

收集完整冷启动 hilog、`getAboutInfo` 内核版本、`chrome://gpu` 与复现步骤。
过滤关键字 `OHOS phone Vulkan`、`OHOS Vulkan`、`OHOS output color`。

手机必须同时出现协商日志与首帧提交日志：

- `negotiated output mode=1 native_format=34`：SDR10。
- `negotiated output mode=2 native_format=34`：HDR10/PQ。
- 实际 `vk_format=64`（A2B10G10R10）或 `58`（A2R10G10B10），对应 Skia 的
  RGBA1010102/BGRA1010102。模式 0 / native_format 12 是 SDR8。
- `first shared NativeBuffer frame presented; active mode=...`，且
  `color_generation` 对应协商结果。只有协商日志、没有首帧日志不算成功。

手机先对真实窗口队列 RequestBuffer，核对实际配置/Parcel，Vulkan 查询及导入、
Skia wrap 成功后才向该输出的 Viz 发布 10 位/PQ 能力。首帧另行确认 draw/present。
颜色与 HDR 元数据写在实际 leased NativeBuffer 上，读取颜色空间核对，
不在仍可能消费旧帧时给整个窗口换 PQ 标签。

平板/PC 日志的 `swapchain format` 必须是 58/64；HDR 的 `color_space` 必须是
1000104008（HDR10 ST2084）。创建 swapchain 后才发布能力；元数据按 swapchain
提交。Vulkan 元数据 API 无读回，不以调用完成代替系统合成器/实屏证据。

系统 HDR 状态、实际亮度与色彩要单独验收。普通截图和录屏常转换为 SDR，
不能据此确认面板 10 位或 HDR。不要只看“色带少了”：抖动也可能有类似效果。

## 本轮行为和限制

- 每个输出窗口固定色彩模式。HDR 模式下普通网页按参考白转换；播放/停止视频
  不热切换整个窗口。改变启动模式需要重启应用。
- 仅不透明输出开放 10 位；独立透明窗口保持 SDR8，页面内部透明元素正常合成。
- 浏览器/renderer 的全局 ScreenInfo、CSS `dynamic-range` 仍保持保守 SDR 声明。
  本轮发布的是每个 Viz 输出的已验证能力。因此网站自动选 HDR 素材不作为启用
  依据；使用已知 HDR 测试素材直接播放，单独核对输入是否保留 10 位/HDR。
- HDR 首版限单显示器。绘制期间每秒检查一次显示器身份与 HDR10 支持；发现
  外接/切换或能力丢失会撤销 HDR 并触发输出恢复。不是无缝多显示器 HDR 支持。
- 协商失败直接 SDR8；10 位运行中失败会锁定本会话为 SDR8，手机允许 GPU
  重启后继续尝试 Vulkan SDR8。驱动故障/超时仍采用已有 GL 回退。
  Chromium 自身恢复策略也可能选择 GL，日志必须记录实际结果。
- `GpuOutputColorFailed` 是会话回退标记，GPU 重启不清除，应用冷启动清除。
  显式 `--ohos-sdr-output` 保持强制 SDR。
- 未改视频 Vulkan SharedImage。当前视频可能仍走 buffer/upload 路径，不能称
  “HDR 视频端到端零拷贝”；最终输出 10 位也不能证明解码/上传没有降到 8 位。

## 验收顺序

| 组别 | 配置 | 必查 |
|---|---|---|
| A | 默认单进程 GL | 网页、键盘、视频、弹窗基线 |
| B | 独立 Vulkan + SDR8 | 原有多进程体验与性能无回归 |
| C | 独立 Vulkan + SDR10 | 实际 10 位分配、通道顺序、渐变有效位 |
| D | 独立 Vulkan + HDR10 | PQ、参考白、近黑与高光、色域、系统实屏 HDR |
| E | 故障/生命周期 | GPU 崩溃、旋转、前后台、关闭/重建窗口、外接显示器 |

每组核对内核版本；至少 3 次冷启动、20 轮标签开关、5 次受控 GPU 崩溃恢复，
网页滚动、GLES WebGL、输入框及独立透明弹窗均纳入。对 D 组反复切换 SDR 网页、
已知 HDR 素材和黑屏测试图，确认网页白底没有变成峰值亮度，停止视频后没有偏色。

精度测试使用 10/16 位源和超过 256 级的渐变/固定像素值，通过测试专用 GPU
读回或帧捕获核对位序、有效位和裁剪；8 位 PNG/CSS 色块只用于色序与布局检查。
HDR 素材需记录 P010/10 位解码、输入 primaries/transfer/range、上传格式和
合成信息，任一环节降为 8 位不能计为“端到端 10 位”。

性能按 120 Hz 的 **8.33 ms** 预算比较 B/C/D：帧间隔 P50/P95/P99、超预算比例、
实际掉帧、GPU 时间、队列等待、FD/内存增长、温度与功耗。普通帧没有新增 CPU
像素读回/整帧 blit；启动探测及原有 resize/清理可以同步等待，不计作稳态帧路径。

不通过时保留首次失败前后的完整日志，注明请求模式、实际模式、native/Vk/Skia
格式、color generation、是否出现首帧日志、是否回到可浏览的 SDR。
