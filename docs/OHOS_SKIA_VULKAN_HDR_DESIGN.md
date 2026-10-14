# OHOS GL / Skia Vulkan 亮度、10 位及 HDR 输出

目标：由引擎读取系统亮度，GL 和 Vulkan 共用；独立 Skia Vulkan 默认自动尝试
10 位输出。应用默认仍使用原有进程模式。外壳操作见
[测试说明](OHOS_SKIA_VULKAN_HDR_TESTING.md)。

## 系统亮度公共层

SDK `@ohos.display.d.ts` 从 API 22 提供公开接口 `display.getBrightnessInfo(displayId)`
以及 `display.on/off('brightnessInfoChange', callback)`，查询不要求额外亮度权限。
引擎 NativeContext 在 Chromium 启动前读取一次，监听后续变化；显示器变化和返回
前台时重新读取，销毁时注销监听。事件直接更新公共状态，不人为延迟亮度响应。

- `sdrNits` 是系统提供的 SDR 白亮度，作为 Chromium SDR reference white。
- `currentHeadroom` 用于当前 HDR 输出峰值：`sdrNits × currentHeadroom`。
- `maxHeadroom` 只约束上限，不能替代动态 headroom；热限制等情况下不强行使用最大值。
- 只检查有限正数和 PQ 的 10000 nits 编码范围。暗屏低于 80 nits、headroom=1
  均合法，不再使用上一版手动校准范围。
- 旧系统或 API 不可用时，GL 保留既有缺省行为，Vulkan auto 尝试 SDR10。

`DisplayBrightnessBridge → NAPI UpdateDisplayBrightness → OhosScreen →
Chromium DisplayColorSpaces → Viz` 使用现有显示参数通知通道；没有逐帧的系统亮度
查询、文件轮询或额外亮度 IPC。GL 保留已有 EGL 格式、视频元数据及呈现方式，
新增参数影响 HDR 色彩转换与 tone mapping。

## Vulkan 默认策略

`vulkanOutputMode` 默认 `auto`。HDR10 显示器且存在系统亮度时尝试 HDR10，否则
尝试 SDR10；显式 `sdr8` 保留为关闭入口。只有 separate GPU 的 Skia Vulkan 使用
这个输出策略。手机仍要求 experimentalPhoneFork；平板/PC 仍走官方子进程。

模式在输出创建时固定，亮度实时更新。实际 NativeBuffer allocation / Vulkan import
或 WSI 格式对验证成功后，输出才向 Viz 发布自己的格式。独立透明窗口保留 RGBA8，
不把两位 alpha 用于透明根窗口。浏览器全局 ScreenInfo 不因此承诺 Vulkan HDR，
以兼容 GL 回退；亮度数值通过显示参数继续传递。

`vulkanHdrWhiteNits`、`vulkanHdrPeakNits` 是可选固定调试覆盖。正常外壳不填写，
运行中亮度变化也不修改启动配置，因此不要求重启应用。

## 同一帧的转换与元数据

Vulkan 已协商的格式/色彩空间与系统动态亮度在 Viz 合并。DisplayColorSpaces 更新
会使已有画面失效并重绘；DirectRenderer 把该帧的输出亮度放入 ReshapeParams，
沿 SkiaOutputSurface 的有序 GPU 任务传给输出设备。不能只给 NativeWindow 改标签。

手机继续使用 browser RequestBuffer → GPU 导入并直接绘制 → completion fence →
browser FlushBuffer。亮度变化不改变 VkFormat/尺寸，不清空导入缓存，不同步等待 GPU。
共享通道仅在没有未完成 lease、窗口与 generation 一致、格式仍为 HDR10 时接受新的
亮度；更新绑定到新申请的 buffer，已排队的帧保留各自元数据。原有 FD、尺寸、版本、
代际及 fence 校验保持有效。RGB10A2 仍为 4 B/像素。

平板/PC 使用精确 Vulkan `(VkFormat, VkColorSpaceKHR)` 格式对；HDR10 需要 PQ 和
`VK_EXT_hdr_metadata`。亮度更新作用于现有 swapchain，并保存给后续重建，失败则
进入色彩回退。输出元数据描述合成画面的亮度范围，不把某段视频的 mastering
metadata 当成整个显示器的参数。

## 初始化及回退

已修正 68e8b50 的参数错误：`VulkanInstance::Initialize` 第三个参数是 required
layers，不能传 `VK_EXT_swapchain_colorspace`。现在先绑定 loader、枚举 instance
extensions，再把支持的 colorspace 扩展加入扩展列表；Skia 显式请求零个 layer。

10 位/HDR 目标失败锁定本会话 SDR8；手机可重建 GPU 后继续 Vulkan SDR8。
Vulkan 设备/上下文失败采用既有 GL 回退。全局 SDR fallback 不代表退回单进程。
新应用会话重新尝试，不在失败时反复升级色彩模式。

68e8b50 真机还报告系统 loader 在加载 layer 的动态链接阶段 SIGBUS。匹配 Build ID 的
旧二进制确认调用来自 Dawn VulkanInstance::CreateVkInstance。现存完整 crash 的
maps 确认正在加载 Trace3D，RELRO 复制目标却映射了 Chromium 的 shared-memory-region，
并有 EBADF 日志。fork server 曾关闭链接器缓存 FD，本轮改为保留精确匹配的
relro_cache memfd 及其原始编号；其余应用 FD 仍关闭。320ca09 真机复测已确认该修正
不足以消除 SIGBUS。本轮补充两次 fork、清理和 Vulkan 创建前后的 FD 生命周期诊断，
区分保留条件未命中、fork 回调关闭和后续复用，详见 [诊断记录](OHOS_VULKAN_RELRO_FD_DIAGNOSIS.md)。
构建流程保留按 Build ID 校验的旧二进制符号化。新包通过实际日志确认后端和首帧状态。

320ca09 同时暴露 NativeBuffer 与 NativeWindow 元数据接口的行为差异：NativeBuffer
将元数据首字节转成枚举并校验，SDR 的 NONE(-1) 编码为 255 后被拒绝。SDR8/SDR10
现在只标记 sRGB；HDR10 仍要求类型和静态元数据成功。元数据失败会记录具体步骤和
系统返回码，不把共享协议的 -13 当作权限错误。

按窗口跟踪可能被 HDR 标记的队列，HDR 转 SDR 前必须没有未完成 lease，并调用
NativeWindow CleanCache，包含 HDR 仅部分设置成功的情形。GPU 重启后，新接收线程
等旧线程退还 lease、清理 HDR 队列后再处理新请求；等待发生在后台 MayBlock worker。
已销毁/替换窗口的引用在空闲或新增 HDR 窗口时释放。SDR10 失败不触发 HDR 清理，
不会把浏览器全局置为“SDR 元数据也必须成功”；稳定帧和动态亮度不清理队列。

## 范围与验证

本轮补 GL/Vulkan 自动亮度与 Vulkan 默认 10 位，不实现 Vulkan 视频零拷贝。
Vulkan HDR 暂限单显示器；运行中移除或改变显示器按已有保护回退。HDR10/PQ 以外
的输出格式不在本轮承诺范围内。

自动检查覆盖亮度换算、动态/最大 headroom、低亮度、headroom=1、无效数值、PQ
上限，以及共享协议 FD/颜色/代际校验。ArkTS HAR 编译与 Chromium 154 完整编译
分别验证。真机以正常浏览、HDR 内容、系统亮度调节和日志为主，不要求用户测量
手工校准值或重复整套参数矩阵。120 Hz 的性能预算仍为 8.33 ms。
