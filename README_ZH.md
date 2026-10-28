# Chromium HarmonyOS 适配层

[English](README.md)

本分支用于 Chromium 155 的 HarmonyOS 适配迁移。154 稳定构建保留在 `hmos-154-adapter`。它是独立、非官方项目，不代表 Google、Chromium、华为或 OpenHarmony 官方立场。

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
- `scripts/prepare-depot-155.sh`：在 Depot 中准备独立的 155 源码。
- `.depot/workflows/`：154／155 正式分支使用 Depot CI，各自复用独立源码、工具链和编译缓存，见 [Depot CI](docs/DEPOT_CI.md)。
- `_bak/workflows/`：原 GitHub Actions 工作流备份，不再自动执行。
- `licenses/`：许可证正文与第三方许可证清单。
- `SOURCE_MANIFEST.sha256`：原始源码公开快照的校验清单；当前迁移输入以 Git 提交和迁移清单为准。

源码仓库不包含私有 HarmonyOS SDK 和签名材料。原生运行时与符号归档上传到 Depot CI 产物。

## 通过 CI 构建

将源码改动推送到 `hmos-155-adapter`。CI 在 `/mnt/chromium-155/source/src` 同步固定依赖和工具链、应用迁移补丁，并编译和打包原生运行时。源码、SDK 和编译对象使用独立于 154 的缓存；HAR/HAP 打包仍是单独步骤。

目录和分支约定见 [BUILDING.md](BUILDING.md) 和[迁移说明](docs/CHROMIUM_155_MIGRATION.md)。`scripts/apply-adapter.sh` 保留用于历史 154 源码树。

许可证及对应源码说明见 [OPEN_SOURCE_NOTICES.md](OPEN_SOURCE_NOTICES.md) 与 [SOURCE_OFFER.md](SOURCE_OFFER.md)，DRM/编解码边界见 [docs/DRM_AND_MEDIA.md](docs/DRM_AND_MEDIA.md)，账号与同步信任模型见 [docs/HARMONYOS_SYNC.md](docs/HARMONYOS_SYNC.md)。
