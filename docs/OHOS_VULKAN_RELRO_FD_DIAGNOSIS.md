# 68e8b50 Vulkan layer SIGBUS 与继承 FD

2026-10-14 读取手机上已经存在的完整 native crash，未重启应用、改配置或触发复现。
对应设备为 Mate 70 Pro+ / API 26，内核为 68e8b50。
原始文件保存在本机 `artifacts/68e8b50-relro-crash/native-crash.log`，不提交原始设备日志。

## 已确认的证据

- 旧 libweb_engine.so Build ID 为 `eb1716d89d2124f385e7e379c326e70124edc80e`，
  `0x33b2894` 符号解析为 `dawn::native::vulkan::VulkanInstance::CreateVkInstance`。
- SIGBUS(BUS_OBJERR) 位于 musl `do_prelink` 调用的 memcpy；目标 x0 为
  `0x5a04f7b000`，源 x1 为 `0x5a7ae4a000`，长度 x2 为 `0x1000`。
- maps 显示源位于 `/system/lib64/libVkLayer_Trace3D.so`。
- 目标映射为 `5a04f7b000-5a04f7d000 rw-p 0003e000 /memfd:shared-memory-region (deleted)`，
  并非链接器 RELRO 缓存。原有 RELRO 映射名为 `/memfd:relro_cache (deleted)`。
- 崩溃前有 `prelink: relro sharing mmap failed, errno=9` 以及
  `prelink: relro partial sharing mmap failed, errno=9`。errno 9 是 EBADF。
- OpenFiles 中未找到 relro_cache；低编号 FD 已出现 shared-memory-region。
  无法通过 shell 读取仍存活父进程的 `/proc/PID/fd`（Permission denied）。

## 原因判断和修正

fork server 的 CloseInheritedFds 关闭除控制通道、标准流和目录之外的全部 FD，
但 fork 快照保留了系统链接器的 prelink 状态。已关闭的缓存编号可以先产生 EBADF，
随后被普通 Chromium memfd 占用；以 RELRO 偏移映射较短的无关文件，访问时可触发 SIGBUS。
现有 maps 与这一机制高度吻合。没有系统私有链接器源码和修正后真机结果，
不能把该机制的每个内部细节视为已直接观测。

修正只保留 `/proc/self/fd` 精确指向 `/memfd:relro_cache (deleted)`，且 fstat 确认为
无目录链接的普通文件的 FD。保留原编号及原标志，不能关闭后重新复制到高位。
普通 memfd、socket 及名称仅带 relro_cache 前缀的文件仍关闭；Chromium 接收 FD
继续停放在继承 FD 高水位以上。该路径仅用于显式启用的手机 fork server。

不禁用系统 Trace3D layer，不改变系统 loader，不要求额外屏幕权限。
Skia 扩展误传 required_layers 是另一个已修正的初始化问题，不与 Dawn 的 SIGBUS 混为一谈。

## 验证与交接

`scripts/test-ohos-fork-server.sh` 新增真实 fork server / dlopen 回归场景：两个模拟
relro_cache 在原编号保留；测试 DSO 的 constructor 创建 32 个短共享内存文件模拟
低 FD 复用，然后按原缓存 FD 映射 `0x3e000` 偏移并读写，核对文件 inode 和 CLOEXEC。
连续三个子进程验证保留有效，并验证普通共享内存、相似名字 memfd、socket 被清理，
浏览器自己的 FD 未被关闭。原有 IPC、100 次启动、崩溃回收等测试一并执行。
本机 Linux 容器中，修正版本全部通过；保留相同测试、换回修正前的 production
fork server 后，仅新增的 RELRO 保留测试失败，确认该测试能检出旧行为。

真机由外壳替换新 HAR 后按 [测试说明](OHOS_SKIA_VULKAN_HDR_TESTING.md) 正常测试。
先确认 GPU 不再因 layer 加载退出，再确认 Vulkan instance、实际输出格式和首帧。
10 位/HDR 成功不能仅由无崩溃或配置值判断。
