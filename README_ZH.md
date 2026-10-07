# Chromium HarmonyOS 适配层

[English](README.md)

本分支公开本地 Chromium 150 HarmonyOS 测试 HAP 使用的通用适配代码。它是独立、非官方项目，不代表 Google、Chromium、华为或 OpenHarmony 官方立场。

## 基线

- Chromium：`150.0.7871.114`
- 上游提交：`f405107495a07cb1bfcf687d4af8d91117098db6`
- 目标系统：`ohos`，产品界面统一表述为 HarmonyOS
- 目标架构：`arm64`
- 单一产品：`plan_kirin_pc`
- V8：构建包含 JIT 编译器，启动时探测能否映射可执行内存，能则启用 JIT（平板、2in1），不能则以 JITless 运行（手机正式包）。JITless 时没有 WebAssembly：所需的 DrumBrake 解释器未编入
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

- `patches/chromium-154-harmonyos.patch`：对 Chromium 上游已跟踪文件的完整修改。
- `overlay/`：新增的 HarmonyOS、ArkWeb、Ozone、Aura shell 与 ArkUI 源码。
- `external/deps_code/webview/`：适配层需要的 WebView 接口源码。
- `config/args.plan_kirin_pc.gn`：本次原生构建使用的 GN 参数。
- `scripts/apply-adapter.sh`：将适配快照应用到指定 Chromium 基线。
- `.github/workflows/upstream-stable.yml`：检查 Chromium Stable，并可触发带授权 SDK 的自托管构建。
- `licenses/`：许可证正文与第三方许可证清单。
- `SOURCE_MANIFEST.sha256`：除清单自身外，所有公开常规文件的 SHA-256。

本分支不发布 HAP、原生编译产物、HarmonyOS SDK、签名证书、Profile、设备 UDID、私钥或本机签名配置。开发者需要自行安装 HarmonyOS SDK，并配置自己的签名。

## 应用适配

准备完整 Chromium checkout，切换到上述固定提交并同步 DEPS，然后执行：

```bash
./scripts/apply-adapter.sh /path/to/chromium/src
cp config/args.plan_kirin_pc.gn /path/to/chromium/src/out/plan_kirin_pc/args.gn
```

继续构建前请阅读 [BUILDING.md](BUILDING.md)。许可证及对应源码说明见 [OPEN_SOURCE_NOTICES.md](OPEN_SOURCE_NOTICES.md) 与 [SOURCE_OFFER.md](SOURCE_OFFER.md)，DRM/编解码边界见 [docs/DRM_AND_MEDIA.md](docs/DRM_AND_MEDIA.md)，账号与同步信任模型见 [docs/HARMONYOS_SYNC.md](docs/HARMONYOS_SYNC.md)。
