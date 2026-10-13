# 手机独立 GPU 的 Skia Vulkan 实验

范围：phone/foldable，显式开启 `experimentalPhoneFork`，GPU 独立进程。
默认仍为单进程，不启用手机单进程 Vulkan。平板/PC 继续原有官方子进程和
Vulkan swapchain 路径。本实验不切换 ANGLE Vulkan。

## 外壳配置

启动内核前设置，保存后必须完整退出并重启应用，不能只刷新网页：

```ts
config.experimentalPhoneFork = true;
config.gpuProcess = 'separate';
config.angleBackend = 'gles';
config.skiaBackend = 'vulkan';
```

保留现有已验证的 `jitless` 配置，不同时改变 JIT、WebGPU 或光栅化选项。
不要追加 `--single-process`、`--in-process-gpu`、`--disable-gpu`。
切回对照组只改 `skiaBackend = 'gl'` 并冷启动。

## 如何确认实际启用

同时满足：

- `getAboutInfo` 的内核提交号对应本次构建。
- `chrome://gpu`：`In-process GPU: false`、Skia renderer 为 `GaneshVulkan`。
- 日志包含 `OHOS phone Vulkan: first shared NativeBuffer frame presented`。

只有 `--use-vulkan=native`、Dawn Vulkan adapter 可用或独立 GPU PID，均不算通过。
若显示 GaneshGL，检查 `OHOS phone Vulkan:` 和 `OHOS Vulkan:` 日志，这是回退，
不能计为 Vulkan 测试成功。

## 验收顺序

1. 单进程 GL 基线：确认本次包正常启动、输入、网页和视频。
2. 实验多进程 + separate + GL：与上一版作回归对照。
3. 实验多进程 + separate + Vulkan：确认上述三个启用证据，再测滚动、Canvas、
   WebGL、视频、透明内容、标签切换、前后台、旋转和窗口尺寸变化。检查倒置、颜色、
   残影、黑帧和白屏；至少 20 轮标签开关、3 次多标签冷启动。
4. 在 Vulkan 已上屏后使用已有受控 GPU 崩溃入口：新 GPU 应在本会话回退到 GL，
   网页可恢复，日志说明 `unavailable or failed earlier; use GL`。完整退出重启后
   才重新尝试 Vulkan。回退仍失败或循环重启即为不通过。
5. 同页面、温度、电源模式比较 GL/Vulkan：120 Hz 的帧预算为 **8.33 ms**，记录实际
   显示帧率、帧间隔 P50/P95/P99、超过预算比例、FD 和内存趋势。区分队列等待、
   GPU 执行与提交；提交次数和提交 ACK 不是实际上屏帧率。

失败时保留 GPU 报告、完整启动至故障日志、截图/录屏、trace 和配置。
回退是容错成功，不代表 Vulkan 功能通过。长期稳定性由真机验收。

## 实现与边界

主进程 RequestBuffer，传 NativeBuffer 元数据/FD 和 acquire fence；子进程通过
`VK_OHOS_external_memory` 导入 RGBA VkImage，Skia 直接绘制。绘制前等待 acquire
semaphore，提交时释放到 GENERAL / FOREIGN queue family，再导出 SYNC_FD completion
fence，由主进程 FlushBuffer。Vulkan 成功导出时的 -1 表示已完成，不是导出失败。
成功输出路径不读回像素、不 CPU 拷贝、不做最终整帧 blit；不代表网页所有渲染阶段
都没有拷贝，WebGL/视频的跨 API 共享仍须单独测试。

缓存最多 8 个导入；每次使用不同 lease 时全帧重绘，不依赖内容保留。正常帧使用 GPU
同步；resize/缓存清理/销毁等待已提交绘制完成后释放 VkImage 和 NativeBuffer。
窗口暂未注册或尺寸未对齐时临时绘制后丢帧，下帧重新申请，不把它误判为 Vulkan 故障。
不支持导入、同步错误或呈现错误时记录阶段并退出 GPU，由现有 GPU 恢复接管。
导入超时 2 秒终止子进程；浏览器将未确认完成的 lease detach，并锁定本会话 GL 回退。
任何尝试过 Vulkan 的 GPU 代际结束均采取同样的保守回退策略。

实际显示时间仍未知，反馈使用提交确认，刷新间隔从主进程 NativeVSync 获取。
私有 Parcel 布局、驱动 NativeBuffer 导入和跨 API 资源兼容性仍需真机确认。
