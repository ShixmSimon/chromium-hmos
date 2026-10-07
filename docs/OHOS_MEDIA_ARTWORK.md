# 鸿蒙媒体卡片封面

状态：实现中，等待 CI 编译及协调者真机验收。

## 已核实接口

通过只读 CI 任务 [37579946233](https://github.com/miramira8295/chromium-hmos/actions/runs/37579946233) 取得 runner 的当前源码和实际 SDK 头文件。

- `native_avmetadata.h` 提供 `OH_AVMetadataBuilder_SetMediaImageUri`，未提供直接接收 PixelMap 的封面接口。
- `oh_file_uri.h` 提供 `OH_FileUri_GetUriFromPath`，结果要求调用方 `free()`。现有 AVSession 代码已记录系统分配器与 PartitionAlloc 混用问题，本实现通过已有 ArkTS 桥接使用 `fileUri.getUriFromPath`，不引入这类跨分配器释放。
- 本组件构建元数据的方法是 `UpdateDisplay()`。Chromium 通知层另有 `UpdateMetadata()`，两者不能混淆。
- 当前 Chromium 通知层使用 `hide_metadata` 处理无痕会话。仅沿用原有占位图替换仍会产生图片文件，因此 OHOS 分支直接清除封面，不写无痕占位图。

## 实现

引擎 HAR 注册 `mediaartwork` 服务。服务在注册时异步初始化应用 `cacheDir/chromium-media-artwork`，删除该目录内符合 `artwork-<十六进制编号>.png` 的历史文件。初始化在进程内只执行一次，多窗口共用结果；清理完成前不允许写入新封面。目录初始化或 URI 转换失败时返回不可用，不记录异常中的路径。

内核调用 `mediaartwork.location` 获取真实缓存目录和对应文件 URI。使用 `fileUri.getUriFromPath` 转换并保留转义后的路径，移除包名 authority，生成 `file:///data/storage/...`。原因是公开的 [OHAVSession.cpp](https://github.com/openharmony/multimedia_av_session/blob/master/frameworks/native/ohavsession/src/OHAVSession.cpp) 与 [OHAVUtils.cpp](https://github.com/openharmony/multimedia_av_session/blob/master/frameworks/native/ohavsession/src/OHAVUtils.cpp) 显示 C SDK 在应用进程通过 libcurl 读取图片后设置 PixelMap；带包名的 `file://<包名>/...` 不能直接交给普通 libcurl 当本地文件。此判断基于公开实现，仍需目标 HarmonyOS 版本验证。

位图缩放、PNG 编码、写入及删除都在 `base::ThreadPool` 的 sequenced task runner 上执行，最长边限制为 512px，保持宽高比，不放大小图。每次生成随机文件名，不复用系统可能缓存的旧 URI。

代次号在收到新图、清除、切换媒体会话及销毁时更新。后台任务在编码和写入前后检查代次，UI 回调再次检查；不再使用的文件由持有者析构后排队删除。已发布的旧文件保留到新元数据提交成功；清除、停止和销毁路径直接释放所有本组件持有的封面文件。

`UpdateDisplay()` 始终设置图片 URI，包括清除时的空字符串。网页不提供封面时 OHOS 不写产品占位图，避免把上一首图片留下。无痕及未知会话状态不提交图片编码任务。切换会话或清除元数据时同时停止通知层的延迟图标更新。

无痕策略是**不写任何无痕封面或无痕占位图**。切入无痕前已有的普通会话文件会异步清理；目录本身可以存在。进程被强制终止来不及清除的普通模式文件，由下一次引擎服务注册时清理。

新增上游补丁只涉及 `components/system_media_controls/BUILD.gn` 的 codec 依赖和通知层 OHOS 分支。以 runner 快照为基线，工具生成 `git diff --no-index`，在本地隔离目录模拟增量脚本应用；不修改 runner 源码，不在本地编译。

## 尚需实测的边界

- `fileUri.getUriFromPath` 负责转换格式，不代表获得跨进程访问授权。本实现依赖 C SDK 在应用进程内读取本地文件；目标 HarmonyOS AVSession 是否遵循这条路径，必须真机确认。
- `OH_AVSession_SetAVMetadata` 返回成功只表示提交成功，不证明卡片已经显示图片；平台可能异步读取。需特别检查切换、清除后是否出现系统侧的迟到图片。
- 不直接使用网页远程 artwork URL，不向系统发起额外的带 Cookie 下载，也不新增文件共享权限。
- 适配层新增日志只记录通用结果，不记录图片 URI、文件路径、标题或网页地址。

## 验收

由协调者操作设备；测试前先确认设备可用，并征得用户同意。使用 `docs/test-pages/media-artwork.html`，音源是本地生成的全零 PCM WAV，无自动播放，不播放有声音的媒体。

1. 普通窗口开始无声播放，系统卡片显示红色 A，切换后显示蓝色 B。
2. 无封面、快速 A → B → 无封面，不能恢复旧封面。
3. 系统播放/暂停、前后台切换后封面正确；暂停期间允许保留当前封面。
4. 停止并清空元数据、关闭页面、关闭会话后，缓存文件最终被删除。
5. 编码期间切歌、关闭标签、切到无痕或退出，旧任务不能覆盖新图或遗留文件。
6. 无痕窗口播放 A/B 后，缓存中不新增封面文件。
7. 模拟应用异常退出，再次启动时清除旧缓存，不误删其他应用缓存。
8. 分别验证手机 `--single-process` 和平板/PC 多进程。浏览器进程负责媒体会话，但不能以此代替设备验证。

CI 编译只验证代码和引擎 HAR；不能代替 URI 可读性、卡片行为和缓存生命周期实测。
