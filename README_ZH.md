# Chromium HarmonyOS 适配层

[English](README.md)

面向 HarmonyOS 的 Chromium 适配层，为 [Lumie 浏览器](https://github.com/miramira8295/Lumie)提供内核。本仓库维护原生平台补丁、系统集成源码、引擎桥接及构建工具；Lumie 的应用界面在独立仓库维护。

当前主用分支为 **`hmos-155-adapter`**，基于 Chromium **155**；`hmos-154-adapter` 保留早期 154 适配。这是独立、非官方项目，不代表 Google、Chromium、华为或 OpenHarmony 官方立场。

[构建指南](BUILDING.md) · [引擎下载](https://github.com/miramira8295/chromium-hmos/releases) · [原生构建 CI](https://github.com/miramira8295/chromium-hmos/actions/workflows/incremental-build.yml) · [第三方许可](OPEN_SOURCE_NOTICES.md)

## 能力与当前状态

| 方向 | 集成内容 |
| --- | --- |
| 设备与界面 | 手机、平板及 2in1 路由，ArkUI 外壳桥接和 Chromium Aura/Views 集成。 |
| JavaScript 与 WebAssembly | 启动时探测可执行内存能力，选择 JIT 或 JITless；JITless 下通过 DrumBrake 执行 WebAssembly。 |
| 图形与媒体 | OpenGL/Vulkan 适配、HarmonyOS 媒体解码与 HDR 接入；格式和 HDR 可用性取决于设备、系统及渲染路径。 |
| 开发者工具 | Chromium DevTools 与本机 CDP 调试；Lumie 提供手机菜单和可展开、收起的悬浮入口。 |
| 内容防护 | 原生广告过滤、本地防钓鱼规则集成及 Chromium 签名数据组件更新。 |
| 引擎分发 | CI 发布未签名 `engine.har`、原生符号及校验清单，由下游应用打包签名。 |

### Chromium 数据组件

`chrome://components/` 按明确白名单接入八项组件：CRLSet、PKI Metadata、Safety Tips、Certificate Error Assistant、File Type Policies、Origin Trials、Hyphenation 和 Crowd Deny。更新保留 HTTPS 和 Chromium 固定发布者密钥的 CRX 签名校验。Origin Trials 更新后在重启时读取；Crowd Deny 为 Lumie 的通知权限询问提供本地风险提醒。

组件已注册不代表数据已下载或生效。2026-11-05 的官方 OHOS 更新检查提供了 Hyphenation 和 Crowd Deny；File Type Policies、Origin Trials 返回“无更新”，保留内置策略和默认配置。集成后的 155 原生构建及 Lumie 打包已通过，真机激活验证状态见[组件说明](docs/OHOS_SECURITY_COMPONENT_UPDATES.md)。

独立视频图层提升仍属实验能力，Lumie 当前关闭此路径，视频由 Chromium 网页合成器合成。详见[视频图层说明](docs/OHOS_VIDEO_OVERLAY.md)。内核不包含 Widevine CDM，编解码及 DRM 边界见[媒体说明](docs/DRM_AND_MEDIA.md)。

## 基线

- Chromium：`155.0.8059.91`
- 上游提交：`f043649a0ef1fd96e48d56ddbfc8a4a99aa2ca37`
- 目标系统：`ohos`，产品界面统一表述为 HarmonyOS
- 目标架构：`arm64`
- 单一产品：`kirin_pc`；CI 输出目录为 `out/ohos_arm64`
- V8：构建包含 JIT 编译器，启动时探测能否映射可执行内存，能则启用 JIT（平板、2in1），不能则以 JITless 运行（手机正式包）。JITless 时 WebAssembly 由 DrumBrake 解释器执行
- UI：大屏使用 Chromium Aura/Views；手机使用 ArkUI 浏览器栏和 Chromium 内容层
- 媒体：测试配置启用 AAC/H.264 构建开关；不包含 Widevine CDM
- 公开身份：Chromium HarmonyOS Adapter（`com.caidingding233.chromium`）；商业品牌和 AppGallery 签名保留在独立下游

## HarmonyOS 集成

- 手机和折叠态窗口使用手机 UA 与 ArkUI 浏览器栏；平板、电脑、智慧屏、汽车和展开态折叠屏使用大屏路由。
- PWA 通过独立且不导出的 `PWASmallAppExperience` UIAbility 启动，使用沉浸式胶囊与 Chromium 原生 PWA 菜单模型。
- ArkUI 桥接 HarmonyOS 分享、投播、打印、文件选择、默认浏览器选择、AppGallery 更新、华为账号和钱包入口。
- Ozone 输入路径区分鼠标与触控，并在生命周期变化时清除陈旧捕获状态，同时转发 HarmonyOS 输入法焦点。
- CDP 与浏览内容 MCP 桥接默认只监听本机回环地址。

## 仓库内容

- `patches/migration-155/`：已重定位到固定 155 提交的主仓与依赖适配；清单记录来源和校验值。
- `patches/chromium-154-harmonyos.patch`：保留的历史 154 基线。
- `overlay/`：新增的 HarmonyOS、ArkWeb、Ozone、Aura shell 与 ArkUI 源码。
- `external/deps_code/webview/`：适配层需要的 WebView 接口源码。
- `config/args.plan_kirin_pc.gn`：本次原生构建使用的 GN 参数。
- `scripts/prepare-chromium-155.sh`：在自托管运行器准备独立的 155 源码。
- `.github/workflows/`：GitHub Actions 适配检查、增量构建、运行时检查与稳定版跟踪；原生构建沿用 `self-hosted, harmonyos-chromium` 运行器。
- `_bak/depot-workflows/`：Depot 工作流归档，不自动执行；历史配置见 [Depot CI](docs/DEPOT_CI.md)。
- `licenses/`：许可证正文与第三方许可证清单。
- `SOURCE_MANIFEST.sha256`：原始源码公开快照的校验清单；当前迁移输入以 Git 提交和迁移清单为准。

源码仓库不包含私有 HarmonyOS SDK 和签名材料。CI 打包未签名 engine HAR，并将 HAR、原生符号和校验清单发布到 GitHub 预发布版本；HAP 签名留在开发机。

## 通过 CI 构建

将源码改动推送到 `hmos-155-adapter`。GitHub Actions 在独立的 `/root/chromium-155/src` 构建原生运行时并打包 engine HAR。两条正式分支共用已有自托管运行器，以同一并发组串行构建；手动运行可选择仅上传验证 HAR，不发布预发布版本。

目录和分支约定见 [BUILDING.md](BUILDING.md) 和[迁移说明](docs/CHROMIUM_155_MIGRATION.md)。`scripts/apply-adapter.sh` 保留用于历史 154 源码树。

许可证及对应源码说明见 [OPEN_SOURCE_NOTICES.md](OPEN_SOURCE_NOTICES.md) 与 [SOURCE_OFFER.md](SOURCE_OFFER.md)，DRM/编解码边界见 [docs/DRM_AND_MEDIA.md](docs/DRM_AND_MEDIA.md)，账号与同步信任模型见 [docs/HARMONYOS_SYNC.md](docs/HARMONYOS_SYNC.md)。
