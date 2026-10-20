# 启动探针真机验证（2026-10-19）

设备：Mate 70 Pro+。在保留应用数据与实验开关的前提下安装 `310f7144`，安装包中的 libweb_engine.so SHA-256 与 CI HAR 一致，外壳 ABC 包含正文请求及遮罩探针。后端由每轮 CDP 确认均为 GraphiteDawnVulkan，恢复 6 个标签。

本轮是 force-stop 后重新启动的进程冷启动，没有清除文件缓存、重启设备或清除用户数据。每轮约 42 秒，保留原有 30 秒 healthy-start settle 窗口。主机启动时间与设备墙钟的差仅用于近似阶段对齐；遮罩到首帧间隔直接由设备 payload 时间戳计算。首帧指 NativeWindow/broker 接受提交，不是屏幕光学呈现时间。

## `310f7144` 的 5 轮采样

单位：ms。

| 轮次 | GPU 初始化 | 正文请求响应 | 遮罩撤下请求（距主机启动） | 首帧提交（距主机启动） | 首帧晚于遮罩 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 1880.1 | 89 | 2011.0 | 3378.9 | 1367.9 |
| 2 | 1675.1 | 71 | 1666.0 | 2701.4 | 1035.4 |
| 3 | 1604.3 | 40 | 1565.0 | 2595.2 | 1030.2 |
| 4 | 1627.0 | 15 | 1528.0 | 2622.0 | 1094.0 |
| 5 | 1546.1 | 40 | 1587.0 | 2653.8 | 1066.8 |

5 轮均恢复页面；每轮有截图及 SystemInfo 记录，无残留启动探测页。截图在启动约 10 秒后获取，不能据此排除启动过程中的短暂黑屏。第五轮 chrome://gpu 报告 GPU process crash count 为 0。另一次在第三轮稳定区间读取后台 chrome://gpu 的 Runtime.evaluate 超时，不作为启动耗时证据。

## 安装后的首次启动：确认正文请求超时

这次不纳入上述 5 轮进程冷启动统计：

- 18:40:24.406，health_start。
- 18:40:25.410，正文 request=1 发出。
- 18:40:26.646，首帧已提交。
- 18:40:29.411，request=1 触发超时，elapsed_ms=4001。
- 18:40:29.913，request=2 发出；26 ms 后返回且 token 匹配。
- 18:40:29.942，要求撤下遮罩，距 health_start 5536 ms。

因此此前 5.5 秒长尾的“被通用正文请求 4 秒超时串行阻塞”已得到直接证据。request=1 未见原生接收标记；该标记只覆盖有 active WebContents 的分支，不能直接推定 IPC 丢包。

## GPU 阶段日志缺口及修复

`310f7144` 的 hilog 和 chrome://gpu 只保留 GPU 初始化起点和初始化后的日志。源码核查：GpuMain 先安装 deferred log handler，Ozone InitializeGPU 中的 AttachOhosGpuChildLogging 随后将它替换为直写 hilog；手机 fork 子进程这段日志未成功采集。

`fe59a37e` 仅在 phone GPU child 已有 handler 时保留它，使初始化日志继续缓存并由浏览器转发。没有修改 GL/EGL 调用、重试、渲染、超时或遮罩逻辑。该诊断修复已通过原生 CI（run 39240010224），并完成安装及两轮真机复测；阶段 begin/end 同时出现在 hilog 和 chrome://gpu，日志缺口已修复。

证据：`artifacts/startup-probes/device-310f7144/`，包括 `hap-verification.json`、`stages.log`、`summary.json`、`run-*.json/png` 与 `chrome-gpu.txt`。


## `fe59a37e` 完整阶段复测

安装包 libweb_engine.so SHA-256 与该版本 CI HAR 一致，Build ID 为 `cc25aa44c958dc7904d7b9d6436788d003c8d8ee`。后端仍为 GraphiteDawnVulkan，均恢复 6 个标签，最终 GPU process crash count 为 0。

| 阶段 | 第 1 轮 elapsed / thread CPU（ms） | 第 2 轮 elapsed / thread CPU（ms） |
| --- | ---: | ---: |
| `GL.StaticBindings` | 0.491 / 0.454 | 0.362 / 0.349 |
| `GL.eglInitialize` | 1468.482 / 243.539 | 1482.869 / 254.153 |
| `GL.NoExtensions` | 1470.019 / 245.074 | 1484.554 / 255.838 |
| `Gpu.CollectGraphicsInfo` | 0.586 / 0.587 | 0.779 / 0.781 |
| `Gpu.InitializeDawn` | 75.608 / 67.440 | 93.684 / 84.329 |
| `Gpu.InitializeAndStartSandbox` | 1553.364 / 319.206 | 1586.740 / 347.909 |

嵌套阶段不能相加。当前 Graphite 后端在 Dawn 内部使用 Vulkan，没有走 Ganesh 的 `Gpu.InitializeVulkan`；缺少该阶段日志属于预期路径。

- `eglInitialize` 的 elapsed 减线程 CPU 分别为 **1224.943 ms / 1228.716 ms**。结合此前系统 trace 的六段 200 ms 休眠与 EGL → GameAwareLayer → samgr 样本，等待已定位到 EGL 初始化这一客户端阶段。
- 仅 elapsed/CPU 差不能区分阻塞、休眠与调度等待；本轮未新增 off-CPU 栈、系统服务 ID 或失败码采样，仍不能把私有系统服务链路的具体原因判死。
- Dawn 只有 **75.608 / 93.684 ms**，不构成这段约 1.2 秒等待的主体。不能因此直接删除 EGL 初始化；它仍涉及 ANGLE/OpenGL 能力和上下文准备。
- 两轮 CDP GPU 初始化分别为 **1557.971 / 1590.586 ms**；这是 GPU 自报总体初始化统计，与 scope 边界不完全相同。
- 两轮正文请求分别 **41 / 113 ms**，没有超时；遮罩撤下请求距主机启动 **1471 / 1664 ms**，首帧提交 **2510.444 / 2627.624 ms**，首帧晚于遮罩 **1039.444 / 963.624 ms**。

两轮均有正常测试页面截图，无残留启动探测页。没有在本轮做 HDR、HEVC 或长播回归；此前的媒体验收结论不由本轮扩展。

## 结论与收尾

已确认 4 秒正文请求超时长尾，以及约 1 秒遮罩／首帧交接空档。GPU 固定等待集中在 EGL 初始化，而非 Dawn 的主要初始化工作。下一步仍应分别处理启动专用 ready/ack、首帧交接，并继续查系统 EGL/GameAwareLayer 的具体服务等待。

本轮实现变更仅为保留 phone GPU deferred 日志处理器，没有改渲染、启动超时、重试或遮罩策略。设备留在 Lumie 原测试页面，日志采集已停止，本轮 CDP 转发已移除；未修改亮度、音量或清除用户数据。手机当前安装的是 `fe59a37e` 内核及外壳启动探针。

完整阶段证据：`artifacts/startup-probes/device-fe59a37e/`，包括 `hap-verification.json`、`stages.log`、`gpu-phases.json`、`summary.json`、`chrome-gpu.txt`、两轮 JSON/截图及 `device-validation-status.json`。原生构建及 HAR 校验信息位于 `artifacts/startup-probes/build-fe59a37e/`。
