# 视频独立图层 / HDR 测试材料

这是本地测试入口，包含自行生成的灰阶和移动方块，无外部视频版权或网络依赖。
页面基线和原生桥接测试已运行；真实硬解 P010 → 独立视频图层及 HDR10 元数据传递已验证，
**网页视频提升和屏幕实际 HDR 亮度输出尚未完成验证**。
详情见 [共享模块实现结果](../../OHOS_VIDEO_OVERLAY.md#shared-native-implementation-and-real-decoder-results)。
现有 RenderService 实验的结果见 [overlay 探针说明](../../../scripts/tests/ohos-video-overlay/README.md)。

## 生成和运行

在 chromium-hmos 根目录执行（Python 标准库；ffmpeg 需包含 libx264、libx265、libvpx-vp9）：

```sh
python3 scripts/tests/ohos-video-overlay/generate_hdr_media.py
python3 scripts/tests/ohos-video-overlay/generate_hdr_media.py --verify-only
python3 scripts/serve-test-pages.py --bind 0.0.0.0 --port 8080
```

电脑访问 `http://127.0.0.1:8080/video-overlay-hdr/`，同一局域网手机访问
`http://电脑局域网IP:8080/video-overlay-hdr/`。现有服务器支持 Range，供 seek 使用。
页面无需 HTTPS、跨域接口或第三方脚本。只需电脑本机测试时绑定 `127.0.0.1`。
生成的 `media/` 文件在本地保留、由 Git 忽略；清单含 SHA256、编码命令、版本和校验结果。
重新生成会覆盖同名测试素材；失败时不保留旧的成功清单。

| 素材 | 用途 | 预期码流信息 |
| --- | --- | --- |
| sdr-avc.mp4 | 8 位基线 | AVC、BT.709、limited range |
| sdr-hevc10.mp4 | 分离 10 位与 HDR 问题 | HEVC Main10、BT.709、无 HDR 静态元数据 |
| hdr10-hevc.mp4 | HDR10 主测试 | HEVC Main10、BT.2020 non-constant / PQ、mastering + CLL/FALL SEI |
| pq-vp9.webm | VP9 10 位 PQ 路径 | VP9 profile 2、BT.2020 / PQ，**没有静态 mastering/CLL 元数据** |

四段均为 1280×720、30 fps、180 帧、6 秒，无音轨。上方八个中性灰块名义亮度为
0、0.1、1、10、100、203、400、1000 nit，下方 80×80 白块移动。HDR 像素按 PQ 公式
计算，再量化到 limited 10-bit `Y=64+876*PQ(L)`，色度为中性值 512；并非给 SDR 像素贴 HDR 标签。
SDR 使用 BT.709 OETF，参考白按 100 nit 建模，后三档封顶；实际 SDR 屏幕亮度不由该数值保证。
HDR10 mastering 为 BT.2020 / D65、0.0001–1000 nit；MaxCLL=1000，MaxFALL 根据图案名义亮度面积计算后向上取整。
有损编码会有量化误差，校验第一帧每块中心的解码 Y 值（容差 4 个码值）。
另外检查全部 180 帧可解码、色彩标签、位深以及 HDR10 SEI。

限制：中性灰用于亮度/传递函数和层次测试，不验证广色域色准、真实内容码率、音画同步、
DRM、HLG、HDR10+ 或 HDR Vivid。码流是 planar yuv420p10le，并不保证手机解码输出就是 P010；
必须读取实际 NativeBuffer 格式。

## 下一轮真机顺序

1. 填写内核版本、设备/系统版本、实际渲染后端、亮度设置和自动亮度状态；先跑 AVC SDR。
   独立图层开/关应分别记录，同一份素材比较。不用屏幕截图亮度判断 HDR 成功。
2. 跑 HEVC SDR10，再跑 HEVC HDR10、VP9 PQ。遇到不支持/加载失败保留错误，不能当作 HDR 或图层通过。
3. 逐项开启网页字幕、弹幕、遮挡，缩小播放器，切换 contain/cover/fill；上下滚动、容器全屏/退出。
   基线可提升独立图层；旋转、圆角等复杂效果允许正确回退，不能覆盖网页文字或越界。
4. 执行 SDR → HDR10 → SDR，观察 HDR 元数据与系统显示状态是否恢复；再测试 seek、暂停、
   快速换源、切换过程中停止、后台/前台、关页。停止后不应被旧异步任务恢复播放。
5. 在真实解码器输出和提交 RS 前采样同一 frame/session 的原生信息（下面的辅助代码）；
   同时采集解码器配置、RS 图层树、独立层提升/回退原因、acquire/release fence 与 allocation/lease 归还记录。
   既有事务回调不等于 presentation feedback；仍不可用时明确记 unknown。
6. 导出页面 JSON；把原生日志、图层证据和显示测量作为独立证据关联。页面报告默认保持
   `nativeEvidence: not-measured`。`dynamic-range: high`、成功播放、requestVideoFrameCallback
   都不能替代这些证据。屏幕录制可能影响合成或经 SDR 转换，不能作为亮度计。

页面默认不做 Canvas/VideoFrame 读回，避免测试本身改变 overlay 条件；网页字幕是普通 DOM。
容器全屏保留字幕。自动切换按播放成功后经过 6 秒触发，属于换源压力测试，不是物理呈现计时。
事件最多保留最近 300 条；导出前保留异常现场，每轮单独导出。

## 原生只读采集辅助代码

`scripts/tests/ohos-video-overlay/hdr_buffer_probe.{h,cpp}` 提供：

```cpp
LumieInspectHdrBuffer(buffer, session_id, frame_id, 0); // decoder output
LumieInspectHdrBuffer(buffer, session_id, frame_id, 1); // before RS submit
```

已在临时调试构建中验证真实解码器输出，**没有接入正式内核**。调试构建在首帧/格式变化调用，
链接 `native_buffer` 与 `hilog_ndk.z`，采集 `LumieHdrBuffer` 日志。普通 SDK ABI 即可，无需 RS 私有 C++ ABI。
调用者必须持有有效 decoder lease，并串行访问（公开接口非线程安全）。函数不映射像素、不消费 fence、
不改 producer-owned 元数据、不延长/释放 buffer 引用。记录 seq、format、尺寸、stride、color space、
HDR type、静态元数据返回码与有界原始字节；getter 失败代表未知，不能推断为 SDR。

公开实现为 metadata 返回 `new uint8_t[]`，辅助代码配对 `delete[]`。头文件没有详细说明释放约定，
因此启用到具体商业固件前需核对对应实现/调试运行；该编译结果本身不证明运行时兼容。
静态布局只在 size 与 SDK 结构体一致时解读，不强行解析其他布局，也不读取无关动态 SEI。
彩色帧/缓冲区提交和 decoder lease 管理仍由后续 presenter 集成实现。

arm64 交叉编译检查（DevEco Studio 默认安装路径）：

```sh
/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/llvm/bin/clang++ \
  --target=aarch64-linux-ohos \
  --sysroot=/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/sysroot \
  -std=c++17 -Wall -Wextra -Werror -fPIC \
  -c scripts/tests/ohos-video-overlay/hdr_buffer_probe.cpp -o /tmp/lumie-hdr-buffer-probe.o
```

## 本地页面回归

启动测试服务器与专用本机 Chrome CDP 后运行（Node 22+）：

```sh
node scripts/tests/ohos-video-overlay/web_smoke.mjs \
  http://127.0.0.1:19333 http://127.0.0.1:8080/video-overlay-hdr/ /tmp/lumie-hdr-web-qa
```

脚本只创建/关闭自己的测试页，测试四段素材解码、尺寸控制、自动切换、停止取消、快速换源和 seek，
保存页面截图及 JSON。运行环境需支持这四种编码；浏览器无法解码会使回归失败，不会静默跳过。
这项回归只验证网页与素材，不代表鸿蒙设备 HDR 或 overlay 验证。

2026-11-03 本机准备验证已通过：四个文件完整解码与清单复核、11 项 Chrome 页面检查、
arm64 辅助代码严格编译。源码和素材哈希见
[hdr-preparation-evidence.json](../../../scripts/tests/ohos-video-overlay/hdr-preparation-evidence.json)。
该准备阶段没有安装代码；后续现有版本真机结果见下节。

## 2026-11-03 真机基线结果

Mate 70 Pro+（API 26 Release），已安装 Chromium 155.0.8059.91，实际为
ANGLE OpenGLES / GaneshGL。两轮在前台运行，页面 wake lock 在结束时释放，测试标签均已关闭；
没有替换 HAP 或开启独立视频 presenter。

- 四段素材均持续播放。AVC / HEVC 为平台解码，原生日志确认硬件 AVC/HEVC；VP9 profile 2 为
  `VpxVideoDecoder` 软件解码。HEVC 10 位在当前 buffer 模式输出 `PIXEL_FORMAT_YUV420P10`，不能称为 P010 Surface 零拷贝。
- HDR10 轨道正确携带 BT.2020/PQ、mastering、MaxCLL=1000 和 MaxFALL=198。解码日志仍保留 BT.2020/PQ。
- **本次会话未开启 HDR 输出**：四段的 `dynamic-range: high` 均为 false；HDR10 播放期间的
  `aura_shellSurface` 队列为 `HDR=0`，静态元数据为空；日志持续报告 global screen SDR fallback。
  解码成功不能记作 HDR 出屏通过。此次没有亮度计测量，也没有调用新 NativeBuffer helper。
- SDR→HDR→SDR、停止取消切换、快速换源/seek 通过；字幕、弹幕、不透明遮挡的真机截图层次正确，
  容器全屏进入/退出成功。旋转、圆角和缩小进行了操作与状态采集，未据此证明独立图层回退。
- 全屏截图仍出现桌面版“按 Esc 退出”提示，作为已有移动端适配问题记录。

精简证据：[hdr-device-evidence.json](../../../scripts/tests/ohos-video-overlay/hdr-device-evidence.json)；
[解码与 SDR fallback 日志](../../../scripts/tests/ohos-video-overlay/hdr-device-decoder-evidence.txt)。
原始测试页报告、媒体事件和截图在 `/tmp/lumie-hdr-device-20261103-run2/`，不含其他网页截图。

下一个验证门槛是定位当前 GL 输出为何未发布 HDR 能力，并接通真实视频 Surface/presenter 后再验证
decoder lease、元数据传递和 HDR 图层。此轮结果不能替代上述尚未接入的链路测试。

复跑设备基线（先建立 CDP fport、本地 HTTP rport；输出目录必须不存在）：

```sh
LUMIE_CAPTURE_TEST_SCREEN=1 node scripts/tests/ohos-video-overlay/device_validation.mjs \
  http://127.0.0.1:19422 http://127.0.0.1:18880/video-overlay-hdr/ /tmp/NEW-OUTPUT DEVICE_SERIAL
```

脚本采集新建测试标签的 CDP Media 事件、页面状态和 RS dump；截图仅在 Lumie 窗口及测试页前台时采集。
不设置浏览器移动端模拟参数。RS dump 是设备范围信息，仅本地保留原始文件，分享前筛选应用节点。

依据：[ITU BT.2100 PQ 定义](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2100-0-201607-S!!PDF-E.pdf)、
[x265 HDR10 SEI 参数](https://x265.readthedocs.io/en/master/cli.html#video-usability-information-options)、
[NativeBuffer API](https://developer.huawei.com/consumer/cn/doc/doccenter-references/api/capi-native-buffer-h)、
[OpenHarmony metadata getter 实现](https://github.com/openharmony/graphic_graphic_surface/blob/master/surface/src/native_buffer.cpp)。
