# Chromium 154 → 155 适配冲突调查执行单

交给执行 agent 的任务：仅调查当前 HarmonyOS 适配层升级到 Chromium 155 的代码冲突，提交证据和报告，不实施升级。

## 1. 授权边界（必须遵守）

- 现有仓库只读：`/path/to/chromium-hmos`（替换为用户提供的实际路径）。
- 允许联网查询 Chromium 官方版本、下载上游源码，以及在新建临时目录中 checkout、试套补丁、生成日志。
- 不修改现有仓库的源码、补丁、配置、索引、分支、HEAD 或 remote；不 stash，不清理用户改动。
- 不 push、开 PR、创建远端 issue、触发 CI；不运行 gn、ninja、hvigor、devecocli build/run，不下载完整 SDK。
- 不操作手机，不安装、不启动应用，不读取应用数据、翻译配置或密钥。
- 不执行自动冲突修复，不选择 ours/theirs，不把试套后的代码回写到现有仓库。
- 不复制签名配置、证书和私有文件。所有输出放在新建临时目录；本任务不负责删除临时目录。
- 遇到已有 agent 的活跃构建/源码目录，不复用或修改它。

## 2. 当前已知信息及陷阱

- 已登记生产基线：`154.0.8037.97`，revision `b510e9d7cd3a2fbd78d0ddc42234103206c5f78d`。开始时只读核对 `scripts/apply-adapter.sh` 和 `upstream-compatibility.md`，不要盲信本文的历史值。
- 当前主补丁为 `patches/chromium-154-harmonyos.patch`，调查文档编写时涉及 732 个文件；重新统计，不能把这个数字当成冲突数。
- 主补丁说明仍提到 `.51` 的导出历史。以 `.97` 为第一对照基线；若 `.97` 试套失败，先报告基线不自洽，不能把已有失败算成 155 新增问题。必要时在另一个临时目录补测 `.51`，不修改版本检查。
- `docs/MIGRATION_154.md` 的 175/570、30.7% 是旧 **150 → 155.0.8059.7** 调查，不是当前 154 → 155；不能引用为本次结论，也不能用 175−158 推算新增冲突。
- 不直接运行 `scripts/apply-adapter.sh`：它会应用补丁、覆盖 overlay，且锁定 154。
- 不直接运行 `scripts/check-upstream-adapter.sh`：它会回写仓库根目录的报告；而且主补丁失败后停止，不能提供完整冲突清单。
- overlay 新文件可以完全没有补丁文本冲突，但因上游 C++ 接口变化而编译失败。文本试套不是编译验证。

## 3. 固定输入，选择目标版本

1. 记录时间、系统、工具版本、仓库当前 HEAD、相关文件的 SHA-256、只读 `git status --short`。当前工作区可能有未提交改动，HEAD 不等于全部输入；不得擅自切换或还原。
2. 从 Chromium 官方仓库/Chromium Dash 查到一个明确存在的 `155.0.x.y` 标签及对应 commit SHA，记录来源链接。
3. 优先使用用户指定的 155 标签；未指定时，选择官方可确认的 155 发布标签并注明选择依据。不要把系统 Chrome 或 Brave 的版本直接当成目标源码基线。
4. 报告中固定完整版本和 SHA，不写含糊的“最新 155”。无法确认目标时先询问，不自己造标签。
5. 对输入补丁、overlay 检查前后做哈希核对；如果用户/其他 agent 中途修改输入，应停止或重新固定输入，不能混合两份快照出报告。

建议先读：`scripts/apply-adapter.sh`、`patches/deps/series`、`scripts/check-overlay-api.py`、`docs/MIGRATION_154.md` 的历史说明。

## 4. 建立独立调查目录

以下是 Bash 示例，只初始化新目录，不在现有仓库 checkout。填写核实后的目标；不要原样执行占位符。

```bash
ADAPTER=/path/to/chromium-hmos
AUDIT_ROOT=$(mktemp -d /tmp/lumie-155-conflicts.XXXXXX)
BASE_TAG=154.0.8037.97
TARGET_TAG=155.0.x.y
mkdir -p "$AUDIT_ROOT/reports" "$AUDIT_ROOT/logs"
```

所有带写操作的命令执行前确认目标确实位于 `AUDIT_ROOT`。不要使用 HOME、工作区根目录作为清理/覆盖目标。源码可能较大；优先 partial clone + sparse checkout，只取补丁涉及文件。记录下载失败、缺文件及空间不足，不将其归类为代码冲突。

### 当前 src 补丁顺序

以实际读取的 `scripts/apply-adapter.sh` 为准；下列是本文编写时的顺序，若不一致，报告差异后使用实际顺序，不改原脚本。

```bash
PATCH_NAMES=(
  chromium-154-harmonyos.patch
  ohos-url-protection.patch
  ohos-url-protection-updates.patch
  ohos-rule-sources.patch
  ohos-extension-updates.patch
  ohos-adblock-rust.patch
  ohos-adblock-execution.patch
  ohos-viewport-translation.patch
  ohos-phone-fork.patch
  ohos-phone-gpu-shared-output.patch
  ohos-ime-explicit-keyboard-request.patch
  ohos-adaptive-vsync.patch
  ohos-phone-skia-vulkan.patch
  ohos-skia-output-format-diagnostics.patch
  ohos-vulkan-sdr-output.patch
  ohos-vulkan-hdr-output.patch
  ohos-phone-vulkan-shared-images.patch
  ohos-vulkan-cached-upload.patch
  ohos-phone-browser-media.patch
  ohos-video-zero-copy.patch
  ohos-phone-vulkan-video.patch
  ohos-webnn-nnrt.patch
  ohos-startup-probes.patch
)
PATCHES=()
for name in "${PATCH_NAMES[@]}"; do
  PATCHES+=("$ADAPTER/patches/$name")
done
```

稀疏路径可从这些补丁的 `diff --git` 头提取，覆盖 a/b 两侧（包括改名），再加入 `/DEPS`、`/chrome/VERSION`。本仓库当前路径没有空格；若后续出现带引号路径，应正确解析，不能简单按空格切分。

分别建立 `$AUDIT_ROOT/src154`、`$AUDIT_ROOT/src155`，示例流程：

```bash
git init -q "$AUDIT_ROOT/src154"
git -C "$AUDIT_ROOT/src154" remote add origin https://github.com/chromium/chromium.git
git -C "$AUDIT_ROOT/src154" sparse-checkout init --no-cone
# 将核实后的路径清单放入该临时仓库的 .git/info/sparse-checkout。
git -C "$AUDIT_ROOT/src154" fetch --depth=1 --filter=blob:none origin "refs/tags/$BASE_TAG"
git -C "$AUDIT_ROOT/src154" checkout --detach FETCH_HEAD
```

155 使用另一个临时仓库、相同路径清单和 `TARGET_TAG` 重复以上流程。记录两边 `rev-parse HEAD`、`chrome/VERSION`，与第 3 节核定值核对。不要运行 checkout hooks 或 DEPS 中的脚本，不做 `gclient sync`。

## 5. 对照组：先验证 154 原始基线

对 `src154` 按顺序执行以下过程。补丁只应用在临时源码中；日志输出到临时目录。

```bash
for name in "${PATCH_NAMES[@]}"; do
  patch="$ADAPTER/patches/$name"
  if git -C "$AUDIT_ROOT/src154" apply --check --binary "$patch" \
      >"$AUDIT_ROOT/logs/154-$name.log" 2>&1; then
    git -C "$AUDIT_ROOT/src154" apply --binary "$patch" || break
  else
    break
  fi
done
```

执行者必须另外记录每一步退出码和首个失败位置；上述循环的最终退出码不能代替逐步结果。

- 全通过：可以进行顺序对比。
- 有失败：保留原始错误，后续步骤标记“前置失败，未验证”。检查是否漏路径、顺序有误、补丁已折叠到主补丁，或现有发布快照与 runner 状态不同。
- 不得为让基线通过而修改现有补丁。基线不通过时可继续做“单补丁结构调查”，但不能声称测得完整的 154 → 155 新增冲突率。

## 6. 155 文本试套与冲突统计

### A. 权威顺序检查

在干净 `src155` 上使用第 5 节的同一顺序，日志前缀换成 `155-`。每一步先 `apply --check`，成功后才在临时目录实际应用；第一次失败就停止该条权威检查链。

记录首个失败补丁的完整错误、受影响路径、失败 hunk 原文及目标源码对应位置。后续补丁不能伪称已成功或已失败，应写“blocked by preceding patch”。

### B. 扩展冲突清单（可选，严格标记不确定性）

若希望尽量列出更多冲突，在另一个全新临时 155 checkout 中做诊断试套，不污染 A 的证据：

- 可用 `git apply --reject --binary` 保留可应用部分和 `.rej`；只作用于这份临时副本，不手工修复。
- 后续失败可能是前置补丁部分缺失造成的连锁失败，必须逐项标记“直接冲突 / 疑似连锁 / 未判定”，不能全部相加当作独立冲突。
- 若对单文件/单补丁进行独立试验，说明它缺少哪些前置补丁；不能直接对裸 155 试套所有增量补丁后宣称全部不兼容。
- 不使用 `--ignore-whitespace` 掩盖问题，不把三方自动合并结果当作语义正确证明。

### 必须使用的计数口径

- 分母：补丁触及的去重文件数。主补丁、src 全部补丁、每个 DEPS 仓库分别统计。
- 分子：实际确认的去重失败文件数。同一文件在多个补丁失败只算一个文件，但保留各补丁的 hunk 明细。
- 区分：文本上下文冲突、文件删除/改名、已存在/重复应用、二进制补丁问题、前置连锁、下载/稀疏缺失。
- hunk 数和文件数分别报告。只拿到首个失败补丁时，写“已确认下限”，不是全量结果。
- “没有冲突”和“没有测试到”严格分开；任何未完成项都列出来。

## 7. DEPS 子仓库单独验证

读取实际 `patches/deps/series`，目前包括 V8、ANGLE、Dawn、FFmpeg、PDFium、Perfetto、Skia、Vulkan Loader、WebRTC。

1. 分别从 154 和目标 155 的 DEPS 获取每个依赖的真实 URL、固定 revision。静态读取/解析，不执行整份 DEPS；无法解析的变量表达式列为待确认。
2. 在 `AUDIT_ROOT/deps154/` 和 `AUDIT_ROOT/deps155/` 下创建独立临时 Git 仓库，fetch 对应 SHA；不要拿 155 的 Chromium 搭配 154 的依赖版本。
3. 每个仓库先测自身基础补丁，然后按 `apply-adapter.sh` 顺序测额外补丁：
   - Skia：`ohos-skia-native-ycbcr.patch`。
   - Dawn：`ohos-dawn-instance-diagnostics.patch`。
   - ANGLE：`ohos-angle-vulkan-msrt.patch`。
4. 同样先测 154 对照组，再测 155；独立记录文本冲突和未完成原因。
5. 切勿为本次调查同步所有 DEPS、执行 hooks 或启动依赖构建。若成本超出预期，交付阶段性结果并明确未覆盖的仓库。

## 8. overlay 接口变化检查（不编译）

对照 154/155 上游头文件，优先检查：

- `ui/ozone/platform/ohos` 的基类及 override。
- GPU/Viz 输出设备、SharedImage/SharedContext、Vulkan/ANGLE 接口。
- 子进程入口、进程生命周期、Mojo/IPC 相关接口。
- 媒体视频帧、解码和零拷贝路径。
- extensions、浏览器 shell、翻译服务使用的 Chromium 接口。

可运行已有只读扫描器，先检查其当前参数和实现：

```bash
python3 "$ADAPTER/scripts/check-overlay-api.py" --help
python3 "$ADAPTER/scripts/check-overlay-api.py" \
  --overlay "$ADAPTER/overlay" \
  --chromium-src "$AUDIT_ROOT/src155" --include-arkweb \
  >"$AUDIT_ROOT/logs/overlay-api-155.log" 2>&1
```

154 对照组也运行一次，单独保存日志。始终显式传入只读适配仓库的 `--overlay`，不要依赖当前工作目录。

稀疏源码不包含所有头文件，扫描前需在临时目录补取所需头文件及继承链。缺头文件导致的 unresolved 不算 API 已删除。对确认问题附上两版本声明及文件行号。

扫描器主要按方法名检查，不能证明参数、返回值、类型、构造函数及纯虚函数均兼容；输出无错误不能写“编译通过”。所有自动发现均需人工核对基类解析和条件编译，不能仅凭工具措辞宣称确定错误。

## 9. 交付物

全部写到 `AUDIT_ROOT/reports/`，不要改仓库已有报告。

- `SUMMARY.md`：结论、固定输入、目标版本、是否可直接试套、已确认冲突下限/全量数、主要风险、未完成项。
- `conflicts.csv`：repository、patch、file、hunk、category、baseline154、target155、direct_or_cascade、evidence_log。
- `overlay-api.md`：基类/签名变化及两边源码证据，注明静态检查局限。
- `inputs.sha256`：所用补丁与关键检查脚本哈希；记录 overlay 输入状态及核对方式。
- 原始日志、`.rej` 清单、完整 Git SHA 和官方版本来源链接。
- 收尾时只读确认原适配仓库 HEAD、索引及工作区没有因本次调查改变；不要将他人并发修改还原。

报告摘要建议格式：

| 范围 | 154 对照 | 155 结果 | 确认冲突文件 | 连锁/未验证 | 说明 |
| --- | --- | --- | --- | --- | --- |
| src 主补丁 | 待测 | 待测 | 待测 | 待测 | 不填历史 150 数据 |
| src 增量补丁 | 待测 | 待测 | 待测 | 待测 | 按顺序，去重 |
| 各 DEPS 仓库 | 分行填写 | 分行填写 | 待测 | 待测 | 使用各版本固定依赖 SHA |
| overlay 静态接口 | 待测 | 待测 | 不计入文本冲突 | 待测 | 不代表编译验收 |

## 10. 完成标准与停止条件

理想完成：154 对照通过、155 src 顺序检查及补充清单完成、DEPS 分仓检查、关键 overlay API 对比均有证据。

允许阶段性交付：某一阶段被下载、空间、基线不自洽或依赖解析阻塞，列清已完成和未完成项即可；不得通过修复代码扩大范围。

最终回答要明确：**本次仅为调查，没有升级、推送、构建、安装或启动。** 提供报告绝对路径和最值得关注的冲突清单，不承诺未经编译验证的工作量或兼容率。
