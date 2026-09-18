# Chromium 154 适配计划

写给执行 150 → 154 升级的人。读完这份就能动手，不需要回看会话记录。

---

## 1. 现状

| 项 | 值 |
| --- | --- |
| 当前基线 | `150.0.7871.114`（revision `f405107495a07cb1bfcf687d4af8d91117098db6`） |
| 150 线最新 | `150.0.7871.255` |
| 目标版本 | `154.0.8037.51` |

实测的 patch 冲突率（用仓库自带的 `scripts/check-upstream-adapter.sh` 逐版本测出）：

| 目标版本 | 冲突文件 | 占比 | 其中被删/移 | 版本线成熟度 |
| --- | --- | --- | --- | --- |
| 150.0.7871.255 | **2 / 570** | **0.4%** | 0 | `.255` 成熟 |
| 151.0.7922.226 | 64 / 570 | 11.2% | 2 | `.226` 成熟 |
| 152.0.7977.136 | 101 / 570 | 17.7% | 5 | `.136` |
| 153.0.8010.55 | 136 / 570 | 23.9% | 9 | `.55` |
| **154.0.8037.51** | **158 / 570** | **27.7%** | — | `.51` |
| 155.0.8059.7 | 175 / 570 | 30.7% | 11 | `.7` 极早期 |

选 154 而不是 155 的理由：155 的 patch 号只有 `.7`（刚从主干分支出来，156 已在 canary），把生产基线钉在那上面等于替 Google 做 QA；154 冲突率还低 3 个百分点。

**顺手能做的**：`150.0.7871.114` → `150.0.7871.255` 只有 **2 个文件冲突**，却能拿到中间 141 个补丁版本的安全修复。这件事和 154 无关，但值得单独做一次。

---

## 2. 当前 patch 做了什么

### 2.1 两份 patch 的关系

适配层的改动分两处存放，**组织方式完全不同**：

```
patches/
├── chromium-150-harmonyos.patch    ← 主仓库：单体巨型 patch，820KB / 570 文件 / 7847 行
└── deps/                           ← 子仓库：已按仓库切分
    ├── series                      ← 顺序与目标仓库的映射
    ├── v8.patch                              (4.8K)
    ├── third_party_skia.patch                (8.9K)
    ├── third_party_angle.patch               (3.6K)
    ├── third_party_pdfium.patch              (2.8K)
    ├── third_party_webrtc.patch              (1.9K)
    ├── third_party_dawn.patch                (1.0K)
    ├── third_party_ffmpeg.patch              (0.9K)
    ├── third_party_perfetto.patch            (0.7K)
    └── third_party_vulkan-loader_src.patch   (0.4K)
```

`deps/series` 是 quilt 风格的映射表：

```
v8 v8.patch
third_party/angle third_party_angle.patch
third_party/dawn third_party_dawn.patch
...
```

格式为 `<目标仓库路径> <patch 文件名>`，按行顺序应用。

**`deps/` 就是主 patch 该学的样子**——每个 patch 对应一个明确的目标，体量小到能整体读懂，而且改动带注释说明原因。例如 `v8.patch`：

```diff
+} else if (target_os == "ohos") {
+  # HarmonyOS is a musl-based Linux target. Without an explicit target OS,
+  # v8config.h defines an empty V8_TARGET_OS_LINUX that breaks #if checks.
+  enabled_external_v8_defines += [ "V8_HAVE_TARGET_OS" ]
+  enabled_external_v8_defines += [ "V8_TARGET_OS_LINUX" ]
 }
```

一眼就知道改了什么、为什么改。主 patch 做不到这一点。

### 2.2 主 patch 的三类构成

570 个文件按改动性质分（以新增行是否含 `OHOS`/`ohos`/`HarmonyOS` 标识判定）：

**A 类：OHOS 平台适配 —— 496 个文件（87%）**

给 Chromium 增加一个新平台所必需的改动。典型形态是在既有的平台分支里加一条 OHOS：

```diff
     kChromeOsLacros = 7,
-    kFuchsia = 8
+    kFuchsia = 8,
+    kHarmonyOS = 9
```

按模块分布：`chrome` 328、`components` 73、`base` 36、`ui` 32、`third_party` 24、`content` 20、`build` 11、`net` 10、`tools` 7、`gpu` 7、`media` 5、`skia` 3。

**B 类：非 OHOS 标识的改动 —— 74 个文件（13%）**

这类**不是**新平台支持，而是为了在鸿蒙环境下能编译、能跑而打的补丁。细分：

| 子类 | 数量 | 代表文件 | 升级时的处理 |
| --- | --- | --- | --- |
| 功能裁剪 / 编译开关 | 39 | `chrome/browser/accessibility/**/BUILD.gn`、`live_caption/`、`tree_fixing/` | 多为关掉不需要的特性，冲突时按新版结构重做 |
| **upstream 修复** | **22** | `partition_alloc/aarch64_support.h`、`allocator_shim_*`、`base/linux_util.cc` | **先查上游是否已修，修了就删掉** |
| 资源 / 前端 | 7 | `pdf_viewer.ts`、`payments_section.html`、`people_page.ts` | 前端结构变动大，通常要重做 |
| 构建配置 | 6 | `build/config/ozone.gni` 等 | 跟随新版 GN 结构调整 |

**第二子类那 22 个是升级时最容易白干的部分**：它们是 musl libc、aarch64 兼容一类的修复，上游有可能已经独立修好了。逐个确认比机械 rebase 划算得多。

**C 类：混入的非适配层代码 —— 见第 3 节**

### 2.3 patch 之外还有 overlay

`overlay/` 是**新增到源码树的文件**（patch 改已有文件，overlay 加新文件），共 5083 个：

| 目录 | 文件数 | 职责 |
| --- | --- | --- |
| arkweb | 4476 | 华为 ArkWeb 相关（占 88%） |
| ohos | 189 | 鸿蒙资源、构建脚本 |
| third_party | 56 | 第三方适配 |
| **ui** | **25** | **ozone 平台层 —— 渲染接入核心** |
| chrome | 19 | 浏览器层 |
| components | 15 | 组件层 |
| build / cef / content / net / gpu | 21 | 构建配置与嵌入层 |

**关键风险点**：overlay 里的类**继承 Chromium 基类并 override 虚函数**。例如：

```cpp
// overlay/ui/ozone/platform/ohos/ohos_vsync_provider.h
class OhosVSyncProvider final : public gfx::VSyncProvider { ... };

// overlay/ui/ozone/platform/ohos/ohos_surface_factory.h
class OhosSurfaceFactory : public HeadlessSurfaceFactory {
  std::vector<gl::GLImplementationParts> GetAllowedGLImplementations() override;
  GLOzone* GetGLOzone(const gl::GLImplementationParts& implementation) override;
};
```

`ui/ozone/platform/ohos/` 这 19 个文件里就有 **46 处 `override`**。Chromium 内部 API 没有稳定性承诺，跨版本改签名、删虚函数、加纯虚函数都很常见。

**`git apply` 对这类问题完全无感**——overlay 是新增文件，永远不会有 patch 冲突，只会在编译时炸出来。

好消息是范围有限：实测继承 Chromium 基类的文件，核心目录（ui/gpu/content/cef）**3 个**，arkweb **26 个**，合计 **29 个**，不是 5083。

---

## 3. 混入的非适配层代码

### 3.1 问题

公开 patch 的 `components/sync_device_info/device_info.h` hunk 里，**只有 `kHarmonyOS = 9` 属于本适配层**，其余八处来自一个下游集成，与 HarmonyOS 适配无关：

```diff
+             SelfDeleteSupport self_delete_support =
+                 SelfDeleteSupport::kNotSupported);
+  DeviceInfo(DeviceInfo&&);
+  SelfDeleteSupport self_delete_support() const { ... }
+  void set_self_delete_support(SelfDeleteSupport ...) { ... }
+  std::string GetOSString() const;
+  std::string GetDeviceTypeString() const;
+  base::DictValue ToValue() const;
+  SelfDeleteSupport self_delete_support_;
```

判定依据：upstream 原版**没有**这些声明，而它们的实现位于一个不属于本仓库的 `chromium_src` 覆盖目录中；`enum class SelfDeleteSupport` 的定义同样在那里。也就是说 patch 声明了这些接口，却不提供定义。

扫描确认**影响面仅此一处**：把该覆盖目录定义的全部符号提取后在 patch 中逐个搜索，只有 `SelfDeleteSupport` 命中。

### 3.2 为什么关键词检查没拦住

原先的检查规则是对补丁做关键词 grep，确认某个产品名出现 0 次。

这条规则**有结构性盲区**：`SelfDeleteSupport`、`GetOSString`、`ToValue` 这些标识符**不含任何产品名**，关键词检查天然拦不住。当时 grep 确实返回 0，但代码并不属于本适配层。

**替代的验证方法**（必须取代关键词检查）：

```bash
# 从干净 upstream 重放 patch + overlay，再与源码树逐文件比对内容
./scripts/check-upstream-adapter.sh <VERSION> <REVISION>
```

更彻底的做法是**对照 upstream 逐符号验证**：patch 新增的每个标识符，确认它要么由 OHOS 适配引入，要么在 upstream 中已存在。不能依赖名字里有没有某个关键词。

同理，差集验证**只比文件名会漏**：`device_info.h` 在 patch 的文件清单里，文件名对得上，但内容多出 21 行。验证必须比**内容**。

### 3.3 后果

**公开适配层目前单独编译不过**。patch 声明了 `SelfDeleteSupport` 却不提供定义——克隆本仓库按 `BUILDING.md` 构建会撞上 36 个编译错误（`sync_device_info` 9 个，`components/history` 27 个级联）。

### 3.4 处理

还原 upstream 版本后只重新应用 `kHarmonyOS = 9`，随后重新生成 patch 与 `SOURCE_MANIFEST.sha256`。

## 4. 切分方案

### 4.1 为什么必须切

单体 patch 在升级时会让你反复面对同一个问题：**这行改动当初是为了什么？** 570 个文件混在一起，158 个冲突逐个 rebase 时无从判断意图。

Electron 等成熟的 Chromium 下游项目都维护**几十到数百个主题化小 patch**。收益在升级时立刻兑现：

- 每个 patch 独立 rebase，冲突范围小、意图明确
- **上游已原生支持的，直接删掉**（B 类那 22 个 upstream 修复尤其可能）
- 冲突可按主题分派，不是 570 个文件一锅粥
- `device_info.h` 那种非适配层代码混在合法改动里的情况，在小 patch 里一眼可见

### 4.2 切分维度

按**功能主题**切，不是按目录切。建议的分组：

| patch 文件 | 主题 | 大致来源 |
| --- | --- | --- |
| `0001-ohos-platform-identity.patch` | `OS_OHOS` / `IS_OHOS` 平台身份、`build_config.h` | `build/`, `base/` |
| `0002-musl-libc.patch` | musl 适配、allocator shim | `base/allocator/**` |
| `0003-aarch64-support.patch` | arm64 相关修复 | `partition_alloc/` |
| `0004-ozone-platform.patch` | ozone 平台层接入 | `ui/`, `build/config/ozone.gni` |
| `0005-gpu-vsync-raster.patch` | vsync、GPU 光栅化、GL fence | `gpu/`, `ui/gl/` |
| `0006-font-cjk-viewport.patch` | 字体、CJK 回退、移动端 viewport | `third_party/`, `ui/` |
| `0007-feature-trimming.patch` | 功能裁剪与编译开关（B 类 39 个） | `chrome/`, `components/` |
| `0008-upstream-fixes.patch` | **upstream 修复（B 类 22 个）** | `base/`, `partition_alloc/` |
| `0009-resources-frontend.patch` | 资源与前端（B 类 7 个） | `chrome/browser/resources/` |
| `0010-headless-runtime.patch` | 无窗口运行时、keep-alive、同步开关 | `chrome/`, `ohos_nweb/` |

`chrome/` 占 328 个文件（最大头），多数是功能裁剪类，可在 `0007` 内部再按子系统细分。

**`0008` 单独成组很重要**：升级时第一件事就是逐个确认上游是否已修，修了的直接删，这是唯一能让 patch 总量**减少**的一组。

### 4.3 目录结构与 series

参照 `deps/series` 的既有格式：

```
patches/
├── series                      ← 主仓库 patch 的应用顺序
├── 0001-ohos-platform-identity.patch
├── 0002-musl-libc.patch
├── ...
└── deps/                       ← 保持现状不动
    ├── series
    └── *.patch
```

主 `series` 因为都作用于 `src` 根，只需列文件名：

```
0001-ohos-platform-identity.patch
0002-musl-libc.patch
0003-aarch64-support.patch
...
```

**每个 patch 头部必须写明意图**，格式：

```
Subject: [PATCH] ozone: add HarmonyOS platform backend

Chromium has no OHOS ozone platform. This adds the platform backend that
bridges XComponent/OHNativeWindow to ozone's SurfaceFactory and VSyncProvider.

Upstream status: not upstreamable (vendor platform)
```

`Upstream status` 这一行在升级时价值极大——标记 `upstreamable` 或 `may be fixed upstream` 的，每次升级都该复查一次。

### 4.4 切分的执行方式

不要手工拆 820KB 的文本。推荐：

```bash
# 1. 从干净 upstream 重建源码树并应用现有 patch
# 2. 用 git 把改动按主题分批 add / commit
git add <主题相关文件>
git commit -m "ozone: add HarmonyOS platform backend"
# 3. 用 format-patch 导出
git format-patch --no-signature -o patches/ <base>..HEAD
```

这样切分过程本身由 git 保证不丢改动，且每个 commit 天然带 subject 和说明。

---

## 5. 154 适配步骤

### 5.1 前提（必须先完成）

1. **150 基线编译通过** —— 这是唯一能证明清理正确的验证
2. **重新生成干净的 patch**（清除第 3 节所述的非适配层代码）
3. **完成 patch 切分**（第 4 节）

**不要跳过第 2 步直接 rebase 154**：带着已知缺陷去 rebase，等于把它搬进新版本，而且在 158 个冲突的噪音里几乎不可能再发现。

### 5.2 环境准备

构建脚本断言 host 是 **Linux x64**，Windows 必须走 WSL2。

**硬件要求**（基于实测）：

| 项 | 最低 | 推荐 | 说明 |
| --- | --- | --- | --- |
| CPU | 8 核 | 12 核以上 | 实测 `-j 10` 满载 |
| 内存 | 16 GB | **32 GB** | 单个 clang 峰值 **1.25 GB**，10 并行占 5.4 GB；链接 214 MB 的 `.so` 需 4~8 GB |
| 磁盘 | 150 GB | 200 GB | 源码 28 GB + 第三方 23 GB + 每个 out 目录 8~17 GB |

**8 GB 内存的机器不要尝试**：编译阶段勉强，但链接阶段大概率 OOM，而且那一步在 52501 步的**最后**，等于跑十几小时后在终点前挂掉。

**WSL2 配置**（`C:\Users\<用户名>\.wslconfig`）：

```ini
[wsl2]
memory=32GB
processors=12
swap=16GB
```

改完执行 `wsl --shutdown` 生效。

**关键性能陷阱**：源码**必须放在 WSL2 的 Linux 文件系统内**（`/home/<user>/...`），放在 `/mnt/c/...` 会因为跨文件系统 I/O 慢一个数量级。WSL2 的虚拟磁盘默认在 C 盘，空间不够要先迁移 vhdx。

```bash
# Ubuntu 24.04（与当前容器一致）
wsl --install -d Ubuntu-24.04

# 依赖
sudo apt update && sudo apt install -y git python3 python3-pip curl lsb-release file

# depot_tools
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH="$PWD/depot_tools:$PATH"
```

其余依赖参照 `exchange/scripts/30-install-deps.sh`。

### 5.3 获取 154 源码

当前容器里的源码树是**浅克隆**（`.git` 仅 1.5 GB，存在 `.git/shallow`），**不能直接 checkout 154**，必须重新拉。

```bash
mkdir chromium-154 && cd chromium-154
fetch --nohooks chromium
cd src
git checkout -b ohos-154 154.0.8037.51
gclient sync --with_branch_heads --with_tags -D
```

`gclient sync` 会同步 DEPS 里的全部第三方仓库（23 GB 级），耗时以小时计，主要受网络带宽限制。

### 5.4 逐主题 rebase

按 `series` 顺序逐个应用，**一次只处理一个 patch**：

```bash
git apply --check patches/0001-ohos-platform-identity.patch   # 先探
git apply patches/0001-ohos-platform-identity.patch           # 干净就应用
# 冲突则手工解决后 git add / commit，再重新 format-patch 导出
```

处理顺序建议：
1. **先做 `0008-upstream-fixes`** —— 逐条确认上游是否已修，能删就删，先把总量降下来
2. 再做平台身份、musl、aarch64 这些基础层
3. 最后做 ozone、GPU、功能裁剪

### 5.5 编译迭代

```bash
# GN 配置检查（几秒，改 BUILD.gn 后先跑）
CHROMIUM_SKIP_NINJA=1 bash ./ohos_build/build.sh -t n

# 全量编译
./ohos_build/build.sh -t n
```

**注意 JIT 开关**：适配层默认 args 里 JIT 是**关死的**（`v8_jitless=true`、`v8_enable_lite_mode=true`、turbofan/maglev/sparkplug 全 false）。要保留 JIT 必须显式追加：

```
-G "v8_jitless=false v8_enable_lite_mode=false v8_enable_turbofan=true \
    v8_enable_sparkplug=true v8_enable_maglev=true \
    v8_enable_drumbrake=false v8_drumbrake_bounds_checks=false"
```

**编译迭代是真正的工作量所在**。patch 全部 apply 成功只是起点——overlay 的 API 不兼容只会在这一步暴露，预计需要 **3~5 轮**全量编译才能收敛，每轮数小时。

用 `-k 0` 模式（`ninja-keep-going.sh`），一轮跑完拿到全部错误，而不是撞到第一个就停。

### 5.6 验证

编译通过后，按 HANDOVER 第 5、6 节在真机上验证：

- [ ] 应用能启动（注意锁屏会导致启动失败，错误码 10106102）
- [ ] 页面正常渲染，`chrome://gpu` 显示 `rasterization: hardware`
- [ ] 滑动帧率（以真机手感为准，CDP 合成手势测出的帧率不可信）
- [ ] JIT 生效（`chrome://version` 或 V8 相关探针）
- [ ] WebGL 上下文正常
- [ ] 无窗口运行时能起来

---

## 6. 必须保留的调优逻辑

以下都是已经在 150 上调通、并经真机验证的改动。**154 适配时必须逐项带过去**，任何一项丢失都会导致性能或功能回退，而且回退往往是静默的。

### 6.1 JIT

JIT 由**两层**共同决定，任何一层关掉都会退回解释器。

**第一层：GN 编译参数。** 适配层默认 args 里 JIT 是**关死的**：

```
v8_jitless=true   v8_enable_lite_mode=true
v8_enable_turbofan=false   v8_enable_maglev=false   v8_enable_sparkplug=false
v8_enable_drumbrake=true   v8_drumbrake_bounds_checks=true
```

要开启必须显式追加（`build.sh -G`），把上面全部翻转：

```
v8_jitless=false v8_enable_lite_mode=false v8_enable_turbofan=true \
v8_enable_sparkplug=true v8_enable_maglev=true \
v8_enable_drumbrake=false v8_drumbrake_bounds_checks=false
```

**只跑 `build.sh -t n` 会静默退回 jitless**，编译能过、应用能跑，只是 JS 慢几倍——这种失败没有任何报错。

**第二层：应用侧配置。** `chromium-ui/entry/src/main/ets/utils/DeviceType.ets` 里有 `jitless` 配置项，必须为 `false`：

```typescript
jitless: boolean;        // 声明为可配置，而非硬编码
...
jitless: false,          // 实际取值
```

**这一层踩过坑**：源码树里的 `chromium-ui/` 是只在完整构建时刷新的暂存目录，曾经停留在旧版本、把 `jitless` 硬编码回 `true`，导致引擎侧的 JIT 被应用侧一行配置废掉。打包时 chromium-ui 必须从正确的来源取（见第 9 节）。

**平台权限**：`ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY` 官方文档标注仅支持平板和 2in1。但**实测开发者签名的应用在手机上可以申请可写可执行内存并执行生成的代码**（探针验证通过），因此 JIT 可用，前提是不走应用市场分发。用户开启"坚盾守护模式"后会被系统禁掉，**引擎必须能退回解释器**（这正是 `jitless` 要做成配置项而非编译期常量的原因）。

### 6.2 GPU 光栅化

`kDefaultEnableGpuRasterization` 的平台列表里原本没有鸿蒙，落到默认关闭，页面全靠 CPU 画（`chrome://gpu` 显示 `rasterization: disabled_software`）。

```diff
 // gpu/config/gpu_finch_features.cc
 #if BUILDFLAG(IS_APPLE) || BUILDFLAG(IS_WIN) || BUILDFLAG(IS_CHROMEOS) || \
-    BUILDFLAG(IS_ANDROID) || BUILDFLAG(USE_WEBGPU_ON_VULKAN_VIA_GL_INTEROP)
+    BUILDFLAG(IS_ANDROID) || BUILDFLAG(IS_OHOS) || \
+    BUILDFLAG(USE_WEBGPU_ON_VULKAN_VIA_GL_INTEROP)
              base::FEATURE_ENABLED_BY_DEFAULT
```

**验证方式**：真机 `chrome://gpu` 应显示 `rasterization: hardware`。

### 6.3 真实 vsync 与高刷

原本 ozone 层给的是固定 60 Hz 的 `FixedVSyncProvider`，相位取自创建 surface 的那一刻——与屏幕实际刷新完全无关。改为对接鸿蒙原生 vsync，涉及四点，**缺一不可**：

| 项 | 做法 | 缺失后果 |
| --- | --- | --- |
| 真实 vsync | 接 `OH_NativeVSync` 替换 `FixedVSyncProvider` | 相位错乱，持续丢帧 |
| 高刷申请 | `OH_NativeVSync_SetExpectedFrameRateRange` | 鸿蒙默认把应用锁在 60 Hz |
| **窗口绑定** | vsync 连接必须用 `OH_NativeVSync_Create_ForAssociatedWindow` | 不绑定窗口时高刷申请**不稳定生效** |
| DVSync | `OH_NativeVSync_DVSyncSwitch(true)` | 失去自适应垂直同步 |

实现位于 `overlay/ui/ozone/platform/ohos/ohos_vsync_provider.{h,cc}`。

**实测效果**：平均帧间隔 16.6 ms → **8.4 ms**。

**测量注意**：CDP 合成的滑动手势事件频率是固定的，测出的帧率会在 120 和 60 之间跳，**不能代表真机手感**，最终以实际滑动感受为准。

### 6.4 GL fence 降级（GPU 上下文保护）

**背景**：鸿蒙原生 EGL 会偶发地让 `eglCreateSync` 返回 `EGL_NO_SYNC`，同时 `eglGetError()` 却报 `EGL_SUCCESS`（0x3000）——驱动实现不合规。ANGLE 原样转发该失败，`GLFenceEGL::Create()` 返回 null，GL 层抛 `GL_INVALID_OPERATION`，Chromium 据此**丢弃整个共享上下文**，Raster、SkiaOutputSurface、SharedImage 全部级联失效。

实测一次失败会引发 **13 次级联上下文丢失**。触发极低频（真机观测 1 次，270+ 轮脚本复现均未重现），但一旦触发后果严重。上游把这里当作不可能事件处理（`DCHECK`，release 构建直接编译掉）。

**关键事实**：GL sync（`glFenceSync`）走同一驱动但**没有这个缺陷**，实测可用。

**改动**（两处）：

```diff
// ui/gl/gl_fence.cc —— 鸿蒙优先 GL sync
+#if BUILDFLAG(IS_OHOS)
+  if (g_current_gl_version->is_es3) {
+    return std::make_unique<GLFenceARB>();
+  }
+#endif

// gpu/command_buffer/service/shared_image/egl_image_backing.{h,cc}
-  std::unique_ptr<gl::GLFenceEGL> write_fence_
+  std::unique_ptr<gl::GLFence> write_fence_
-    write_fence_ = gl::GLFenceEGL::Create();      // ×2 处
+    write_fence_ = gl::GLFence::Create();
```

第二处顺带补上了一个**原有缺陷**：那两个调用点直接调 `GLFenceEGL::Create()`，从不做扩展检查（而 `gl_fence.cc` 的路径是有检查的）。

**状态：已应用但未经编译验证**（见第 7 节）。

### 6.5 ANGLE 后端选择

鸿蒙上必须用 ANGLE 跑在**原生 EGL/GLES** 之上，不能用 Vulkan：

```
--use-gl=angle --use-angle=gles-egl
```

原因：ANGLE 的 Linux Vulkan display 需要 `VK_KHR_display`，而设备驱动不提供，`eglInitialize` 直接失败、页面全白。上游 ANGLE 和厂商 ArkWeb 都没有 OHOS 的 Vulkan WSI。鸿蒙 EGL 接受 `OHNativeWindow` 作为 `EGLNativeWindowType`，所以 ANGLE 的 DisplayEGL 后端可以直接向 XComponent 窗口呈现。

配套改动：`third_party/angle/.../DisplayEGL.cpp` 中把 OHOS 的 EGL 库名解析为 `libEGL.so`（而非 `libEGL.so.1`）。

实测链路：`Chromium → ANGLE 2.1.1 (DisplayEGL) → 鸿蒙原生 libEGL.so → GPU 驱动`。

### 6.6 无窗口运行时

- `--no-startup-window` 启动后 Chromium 会立刻退出（退出码 0），必须持有 `ScopedKeepAlive`
- `enable_arkweb=false` 时 `WVLOG_*` 宏被编译掉，启动失败**完全静默**——已改用 `OH_LOG_Print`
- 适配层在 `chrome_main_delegate.cc` 中对所有鸿蒙进程追加 `--disable-sync`，自带同步后端的嵌入方需用 `--ohos-enable-sync` 重新打开

### 6.7 平台硬限制（官方文档确认，绕不过）

- **手机上开不了子进程**：`OH_Ability_CreateNativeChildProcess` 在手机返回 `NCP_ERR_MULTI_PROCESS_DISABLED`，仅支持平板和 2in1。因此渲染、合成、GPU 全在一个进程内，**网页崩溃会带崩应用**。
- **系统会回收 GPU 内存**：内存压力下系统会冻结应用并回收其 GPU 页（实测日志 `OnAppFrozen ... gpuReclaimedPages=1766, cost=13597`，冻结耗时 13.6 秒），随后可能直接杀掉进程。压力测试时容易误判为自身 bug。

---

## 7. 遗留状态与待办

**本文档写就时的状态**，供接手时确认：

| 项 | 状态 |
| --- | --- |
| 非适配层代码剥离（源码树） | 已完成，570 文件与 patch 双向差集为 0 |
| 非适配层代码清理（第 3 节） | 已应用于源码树 |
| GL fence 降级（6.4） | **已应用，未经编译验证** |
| 150 纯净基线全量编译 | **未完成**（两轮分别停在 62.7% / 8.8%，主动中止） |
| 公开仓库 patch 重新生成 | **未做**，仓库当前仍含第 3 节所述问题、单独编译不过 |

**建议的接手顺序**：

1. 跑完 150 纯净基线全量编译，确认 0 失败 —— 这是所有后续工作的地基
2. 真机验证第 6 节各项（尤其 6.4 是新改动，此前从未编译过）
3. 重新生成 patch 与 `SOURCE_MANIFEST.sha256`，修掉第 3 节的问题
4. 按第 4 节切分 patch
5. 开始 154 适配

**不要跳过第 1 步直接做 154**。没有一个编译通过的干净基线，154 上出现的任何错误都无法区分是"剥离引入的"还是"版本升级引入的"，排查成本会成倍上升。

## 8. 风险与工作量

| 环节 | 工作量 | 风险 |
| --- | --- | --- |
| 环境搭建（WSL2） | 半天 | 低 |
| 源码获取 + `gclient sync` | 数小时（网络） | 低 |
| patch 切分 | 1~2 天（一次性投入） | 低 |
| patch 冲突 rebase | **158 个文件** | 中，机械但量大 |
| **overlay API 适配** | **29 个高危文件** | **高，真正的未知数** |
| 编译迭代 | 每轮数小时 × 3~5 轮 | 中 |

**总体预期：数天到两周**，主要不确定性在 overlay 的 API 适配。

**开工前建议先做一次侦察**（一小时内可完成，不需要完整源码）：把 overlay 里所有继承 Chromium 基类、override 虚函数的地方列出来（已知 29 个文件），sparse checkout 154 的对应头文件，逐个核对基类是否还在、虚函数签名是否变了。这能把"数天还是两周"这个区间收窄成一个可信的数字。

---

## 9. 已知的坑

**fix 脚本的幂等判断**
必须写成 `if new and new in text`。早期版本写成 `if new in text and old not in text`，当新内容包含旧内容时会重复插入，曾导致 7 处挂接被插两遍。曾有一处全局编译配置被插入两次，而清理脚本只移除第一次出现，留下的那处直接让 GN 解析失败。

**修改正在被 ninja 读取的文件**
必须原子写入（写临时文件后 `os.replace`），否则 ninja 可能读到写了一半的文件。本仓库的清理脚本均采用这种写法。

**差集验证只比文件名会漏**
见第 3.2 节：文件名对得上不代表内容一致，验证必须比内容，且不能依赖关键词。

**引擎产物的来源**
打包时引擎必须从 `out/<配置>/` 或 `exchange/engine-*/` 取，**不要从源码树里的 `chromium-ui/` 暂存目录取**——那个目录只在完整构建时刷新，增量编译后是旧的。曾有一次 `DeviceType.ets` 停留在旧版本，把 `jitless` 硬编码回 `true`，差点让整轮 JIT 优化白做。

**Chromium 内部 API 无稳定性承诺**
overlay 里 46 处 `override` 全是潜在断点。这不是 bug，是跨版本适配的固有成本。

---

## 附录：实测数据

均在 Apple M4 Pro（14 核 / 64 GB）+ amd64 容器（13 核 / 40 GB，Rosetta 跑 x86_64 工具链）下测得。

| 指标 | 值 |
| --- | --- |
| ninja 总步数 | 52501 |
| 全量编译耗时 | 约 5 小时（`-j 10`） |
| 速率 | 小文件段 4~6 步/秒，blink/v8 大文件段 1.2~2.4 步/秒 |
| 单 clang 进程内存 | 峰值 1248 MB，平均约 540 MB |
| 10 并行总内存 | 5.4 GB |
| `libweb_engine.so` | 214 MB |
| out 目录 | 8.3 GB |
| 源码树 | 28 GB（`third_party` 23 GB、`.git` 1.5 GB） |

**性能参照**：同样的活在 x86-64 4c8g 机器上，CPU 侧差 2~3 倍（10 并行 vs 4 并行），但 8 GB 内存会逼到 `-j 2` 并触发 swap，实际差距会拉到 **5 倍以上**，且链接阶段有 OOM 风险。

`git apply --check` 只验证文本能否落地，**不验证编译**。本文所有冲突率数字都是文本层的，实际工作量应按 **3~5 倍**估算。
