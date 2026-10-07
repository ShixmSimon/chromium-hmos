# 鸿蒙媒体卡片封面

状态：修复中。协调者真机确认普通封面可以显示，但无封面时即使清空 URI、更换 assetId、删除缓存，系统卡片仍保留上一张图片。现改用固定中性占位图，等待新版 CI 和真机复测。

## 已核实接口

通过只读 CI 任务 [37579946233](https://github.com/miramira8295/chromium-hmos/actions/runs/37579946233) 取得 runner 的当前源码和实际 SDK 头文件。

- `native_avmetadata.h` 提供 `OH_AVMetadataBuilder_SetMediaImageUri`，未提供直接接收 PixelMap 的封面接口。
- `oh_file_uri.h` 提供 `OH_FileUri_GetUriFromPath`，结果要求调用方 `free()`。现有 AVSession 代码已记录系统分配器与 PartitionAlloc 混用问题，本实现通过已有 ArkTS 桥接使用 `fileUri.getUriFromPath`，不引入这类跨分配器释放。
- 本组件构建元数据的方法是 `UpdateDisplay()`。Chromium 通知层另有 `UpdateMetadata()`，两者不能混淆。
- 当前 Chromium 通知层使用 `hide_metadata` 处理无痕会话。OHOS 分支不编码网页提供的无痕封面，通过清除接口切换到与网页无关的固定浅灰占位图。

## 实现

引擎 HAR 注册 `mediaartwork` 服务。服务在注册时异步初始化应用 `cacheDir/chromium-media-artwork`，删除该目录内符合 `artwork-<十六进制编号>.png` 的历史文件以及旧的 `placeholder-v1.png`。初始化在进程内只执行一次，多窗口共用结果；清理完成前不允许写入新封面。目录初始化或 URI 转换失败时返回不可用，不记录异常中的路径。

内核调用 `mediaartwork.location` 获取真实缓存目录和对应文件 URI。使用 `fileUri.getUriFromPath` 转换并保留转义后的路径，移除包名 authority，生成 `file:///data/storage/...`。原因是公开的 [OHAVSession.cpp](https://github.com/openharmony/multimedia_av_session/blob/master/frameworks/native/ohavsession/src/OHAVSession.cpp) 与 [OHAVUtils.cpp](https://github.com/openharmony/multimedia_av_session/blob/master/frameworks/native/ohavsession/src/OHAVUtils.cpp) 显示 C SDK 在应用进程通过 libcurl 读取图片后设置 PixelMap；带包名的 `file://<包名>/...` 不能直接交给普通 libcurl 当本地文件。此判断基于公开实现，仍需目标 HarmonyOS 版本验证。

位图缩放、PNG 编码、写入及删除都在 `base::ThreadPool` 的 sequenced task runner 上执行，网页封面最长边限制为 512px，保持宽高比，不放大小图。网页封面每次生成随机文件名，不复用系统可能缓存的旧 URI。

代次号在收到新图、清除、切换媒体会话及销毁时更新。后台任务在编码和写入前后检查代次，UI 回调再次检查；不再使用的网页封面文件由持有者析构后排队删除。已发布的旧文件保留到新元数据提交成功；清除、停止和销毁路径直接释放网页封面文件。固定占位文件不随会话结束删除，避免系统异步读取时文件已消失。

`UpdateDisplay()` 只提交带有效图片 URI 的元数据；图片准备好后再提交当前标题等信息，不再尝试用空 URI 清掉卡片旧图。封面结果更新或清除时，assetId 仍使用媒体会话 ID 加封面代次，但真机已证明不能依靠 assetId 改变来清图。

`ClearThumbnail()`、无封面、停止及清除元数据都改为提交固定的 128×128、RGB `#e5e7eb`、完全不透明 PNG。初始化媒体控制器时准备占位图，首次需要时后台生成 `placeholder-v1.png`，同一进程后续复用。内容没有标题、网址、站点图标等网页信息。真实封面编码失败也回退到它；占位图生成失败仅记录通用错误，不无限重试。代次校验同样用于占位图回调，迟到的占位图不能覆盖后来到达的真实封面。

切换会话或清除元数据时同时停止通知层的延迟图标更新。

无痕策略是**不写任何网页提供的无痕封面**；按协调者的真机反馈，允许无痕与普通窗口共用固定占位图。切入无痕前已有的普通会话文件会异步清理；目录和固定 `placeholder-v1.png` 可以存在。进程被强制终止来不及清除的普通模式文件，由下一次引擎服务注册时清理。

新增上游补丁只涉及 `components/system_media_controls/BUILD.gn` 的 codec 依赖和通知层 OHOS 分支。以 runner 快照为基线，工具生成 `git diff --no-index`，在本地隔离目录模拟增量脚本应用；不修改 runner 源码，不在本地编译。

本次占位图修复仅修改 overlay 和文档，不更改上游补丁或新增构建依赖。

## 尚需实测的边界

- 协调者已在测试手机确认网页封面可以显示；其他 HarmonyOS 版本及设备仍需验证本地 URI 路径。
- `OH_AVSession_SetAVMetadata` 返回成功只表示提交成功，不证明卡片已经显示图片；平台可能异步读取。需特别检查切换、清除后是否出现系统侧的迟到图片。
- 不直接使用网页远程 artwork URL，不向系统发起额外的带 Cookie 下载，也不新增文件共享权限。
- 适配层新增日志只记录通用结果，不记录图片 URI、文件路径、标题或网页地址。

## 验收

由协调者操作设备；测试前先确认设备可用，并征得用户同意。使用 `docs/test-pages/media-artwork.html`，音源是本地生成的全零 PCM WAV，无自动播放，不播放有声音的媒体。

1. 普通窗口开始无声播放，系统卡片显示红色 A，切换后显示蓝色 B。
2. 慢速 A → 无封面、B → 无封面以及快速 A → B → 无封面，都必须显示浅灰占位图；关闭再打开控制中心也不能恢复 A/B。随后选择 A/B，真实封面必须恢复。
3. 系统播放/暂停、前后台切换后封面正确；暂停期间允许保留当前封面。
4. 停止并清空元数据、关闭页面、关闭会话后，网页封面 `artwork-*.png` 最终被删除；允许保留固定 `placeholder-v1.png`。系统若仍展示卡片，不应保留网页封面。
5. 编码期间切歌、关闭标签、切到无痕或退出，旧任务不能覆盖新图或遗留文件。
6. 普通窗口 A/B → 无痕播放，卡片必须换成浅灰占位图；缓存中不能新增网页封面文件，只允许固定 `placeholder-v1.png`。
7. 模拟应用异常退出，再次启动时清除旧缓存，不误删其他应用缓存。
8. 分别验证手机 `--single-process` 和平板/PC 多进程。浏览器进程负责媒体会话，但不能以此代替设备验证。

## 验证记录

- 基于 runner 当前源码工具生成补丁，无相邻修改段；使用实际 `apply_incremental_patch()` 在隔离目录验证首次应用、重复应用及从已提交旧补丁升级，结果与目标源码逐字节一致。CI 也实际完成了旧补丁撤回和新版应用。
- 验收页 JavaScript 语法检查通过；生成 WAV 的头信息与长度匹配，PCM 数据全部为零，无自动播放。
- `0cfc618` 的 Incremental build 成功，生成引擎 HAR。没有在本地编译或操作真机。
- 协调者反馈：原版无封面清除验收失败；内核已发送不带图片的元数据、网页缓存也已删除，但系统卡片保持旧封面。此次占位图修复需重新验收，不能沿用原版编译通过作为修复通过的证据。
- 独立的 `Adapter CI` 仍固定使用 Chromium `150.0.7871.114` 检查 154 主补丁而失败。此次改动前的 `2208d35` 已有相同失败，不属于本项修复范围。

CI 编译只验证代码和引擎 HAR；不能代替 URI 可读性、卡片行为和缓存生命周期实测。
