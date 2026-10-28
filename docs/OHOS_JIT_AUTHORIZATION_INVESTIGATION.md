# Mate 70 Pro+ JIT 授权路径调查（2026-10-26）

## 后台恢复后 renderer CodeRange 初始化崩溃（2026-10-29）

手机的浏览器主进程仍存活，但多次新建 renderer 均在进入 V8 后触发
SIGTRAP。匹配 Build ID 的符号文件指向 `InitCodeRangeOnce` 的 OOM
终止路径；系统报告中后续帧来自不可靠的栈扫描，尚未取得失败系统调用的
errno，不能将其定性为物理内存不足或已证实的权限撤销。

原启动探针仅测试一页 RW→RX，且只在主进程启动时运行。
V8 ARM64 external CodeRange 的默认预留为 256 MiB，还会在
`CodeRange::InitReservation` 中执行整段 RWX 和 MADV_DONTNEED。
现改为共享的完整探针：PROT_NONE/MAP_NORESERVE 预留 → RWX → discard
→ 单页 RW 写入 → RX → unmap。不会填满整块预留区域，也不执行生成代码。
探针覆盖默认代码区操作，但不保证任意自定义 V8 参数、地址布局或后续分配成功。

主进程保留既有、显式启用的兼容请求策略；每个 renderer 在 ChromeMain/V8
初始化之前独立复测，不缓存主进程旧结果。失败时只在该 renderer 的已有
js-flags 后追加 `--jitless --wasm-jitless`，避免循环启动即崩溃。
子进程不会新增兼容请求；GPU/utility 的策略保持不变。
失败阶段和 map/protect/discard/unmap errno 写入诊断，不记录 URL 或路径。

回归验证覆盖各阶段失败、所有成功映射的清理、兼容请求后的复测、资源错误
不触发兼容请求，以及显式 JITless 的优先级。真机仍需验证长时间后台及后台
清理后打开多个页面；在完成实测前，不应宣称整个场景已修复。

## 最新结论：已验证自主开启入口（2026-10-26 19:13）

用户提供的 VintagePomeloPro 引用 Wine 的 `ohos_jit_enable()`，调用
`prctl(0x6a6974, 0UL, 0UL, 0UL, 0UL)`。冷启动未观察到微信的同 PID 对照中，
该请求返回0，直接 mprotect 从-22变为0，五种匿名映射全部成功；memfd RX 仍为 EACCES。
退出请求进程后，新建 Lumie 进程仍成功，引擎记录 JIT available。
这与此前无效的 JITFORT 资源初始化子操作5不同。完整证据与边界见文末。
2026-10-27：154/155 增加 `jitCompatibility` 启动配置，由外壳显式启用；Lumie
正常启动选择启用，安全启动或显式 jitless 跳过。内核在 V8/fork server 前按
“探测 → 必要时子操作 0 → 复测”决定，失败保留解释器，详见 SHELL_CONTRACT。
原显式调试入口仍保留。以下冷启动证据来自 10 月 26 日调试版本，不代表新的自动
启动路径、release 签名或其他设备已经验收；仍未证明微信调用同一接口。


## 结论与证据范围

在实机系统 `libappspawn_common.z.so` 中确认微信签名应用标识命中内置 XPM 临时放行名单。
`ConvertIdType` 命中后返回 `PROCESS_OWNERID_APP_TEMP_ALLOW`（10），随后由进程创建路径向内核提交。
这是系统侧为特定应用提供的特殊处理；当前微信包的常规权限列表没有声明两项常见 JIT 权限。

用户报告：启动微信后，杀掉重启 Lumie，探针由 0 变为 1。
2026-10-26 用户随后授权重启，已完成两轮系统启动对照；第二轮取得同一已安装 Lumie 的
**0 → 0 → 1 → 1** 日志：主微信出现前两次为 0，出现后重启 Lumie 为 1，
停止微信后再启动 Lumie仍为 1。关于页截图也确认“已开启（1）”。
微信通知进程在两次 0 之前已经命中临时名单，因此名单命中本身不足以解释这次变化。
这轮尚未确认影响所有应用，也未定位具体触发操作，不能仅由该对照断言“全局 JIT 开关”。
没有更改应用权限或系统安全策略；仅临时启用 AuraShell INFO 日志。


后续同进程实验进一步确认：**Lumie PID 15787 未重启，直接 mprotect 系统调用从
-22 变成 0，停止微信后仍为 0**。因此排除了仅对新建进程生效和仅 libc 包装层改变
这两种完整解释，变化影响到存活进程的内核侧可执行内存判定。
又在另一轮无微信的冷启动中，由 Lumie 自己请求系统 Ark 使用的 JITFORT 初始化：
请求成功、重复返回 EEXIST，但普通 RW→RX 仍为 EINVAL，尚未找到自主开启方案。
具体内核变量及微信触发调用仍未定位；详见文末“同进程定位及自主初始化实验”。

## 实机样本

- HUAWEI Mate 70 Pro+，API 26 Release。
- `uname`：HongMeng Kernel 1.13.0，构建时间 2026-09-20。
- 微信 `com.tencent.wechat`，版本 8.0.22.35，release / normal，SELinux 域 `normal_hap`。
- 微信 `appIdentifier`：`5765880207854244859`。
- Lumie 为 debug 签名，域 `debug_hap`；声明的
  `ohos.permission.kernel.ALLOW_WRITABLE_CODE_MEMORY` 在 `bm dump` 中状态为 0（已授予）。
- 微信声明了 `ohos.permission.kernel.DISABLE_GOTPLT_RO_PROTECTION`，状态为 0；
  这是 GOT/PLT 保护相关权限，不能把它当作 JIT 授权的证明。
- 微信未声明 `ALLOW_WRITABLE_CODE_MEMORY` 或 `ALLOW_EXECUTABLE_FORT_MEMORY`。
  这仅说明安装包权限列表，不排除系统侧名单、签名分发属性等授权渠道。

通过正常 HDC 文件读取取得：

- `/system/bin/appspawn`
- `/system/lib64/libappspawn_helper.z.so`
- `/system/lib64/appspawn/common/libappspawn_common.z.so`

后一个文件是关键样本。SHA-256：
`68bd4a00d743394448cc46914ce4a9900cc1f8876dc80dde182ab46258428354`

ELF Build ID：`f154312994d4d01be027cc02b7ef29d1`。
使用 DevEco SDK `llvm-objdump -d --demangle` 反汇编并追踪寄存器/字符串引用。
以下地址均为这个 ELF 的虚拟地址（非运行时绝对地址），结论限于这个样本。

## 从反汇编还原的关键流程

1. 创建应用进程的路径在 `0x13670` 一带区分签名 debug、无 owner、临时标志和普通应用，
   分别选择 XPM 类型 3、5、10、2；`0x13994–0x139a4` 组装参数并调用 `InitXpmWithParam`。
2. `InitXpmWithParam` 位于 `0x23518`，打开 `/dev/xpm`，提交区域配置；
   在 `0x236cc` 调用 `ConvertIdType(idType, ownerId)`。
3. `ConvertIdType` 位于 `0x23db4`：只有非空 owner 且类型为 2 或 10 才进行名单匹配。
   它读取 `ohos.boot.advsecmode.state`，普通模式查询 `0x29650` 对应的集合；
   普通名单未命中时，还会检查 `const.product.providedDeviceFeatures` 是否含 `large_screen`，
   并查询另一集合。坚盾模式有独立的集合及分支。
4. 名单命中分支在 `0x23f70` 返回 10，普通未命中在 `0x23f94` 返回 2。
   微信标识字符串位于 `0xc1e8`。初始化函数 `0x24368` 在 `0x243a8` 引用该字符串，
   将它构造成第一个字符串项，并在 `0x2462c` 插入 `0x29650` 对应集合。
   这证明它属于普通模式放行名单，证据不限于在文件中搜到一个字符串。
5. `0x23a90–0x23aa0` 提交 `XPM_SET_OWNERID`（命令值 `0x40587802`）；
   记录中包含转换后的类型、owner 标识、目标 API、签名和分发属性。
6. `0x237e4–0x237f4` 在 enableJitFort 非零时提交
   `XPM_SET_JITFORT_ENABLE`（`0x40087803`）。
   它属于 JIT 保护机制配置，不能把名称中的 enable 直接解释为“放开所有应用的 JIT”。

关键逻辑的简化伪代码（省略参数校验、日志、其他名单分支）：

```cpp
owner = trusted_application_signature.appIdentifier;
id_type = debug_profile ? DEBUG : APP;
if (id_type == APP && normal_mode && temporary_allowlist.contains(owner)) {
  id_type = APP_TEMP_ALLOW;
}
configure_xpm_for_spawned_process(id_type, owner, target_api, signing, distribution);
```

owner 的来源参考 OpenHarmony `AppMgrServiceInner::SetAppInfo`：
`startMsg.ownerId = bundleInfo.signatureInfo.appIdentifier`，随后
`AppSpawnClient` 通过 `AppSpawnReqMsgSetAppOwnerId` 传给 appspawn。
这段是公开源码的交叉佐证，未对实机 AppMgr 二进制做同等程度的反编译。

## 常规权限链与微信名单链

公开实现还存在另一条路径：AccessToken 的 `GetKernelPermissions` →
`jitPermissionsMap` → appspawn 扩展消息 `MSG_EXT_NAME_JIT_PERMISSIONS` →
进程内核权限配置。不能只查 module 的 requestPermissions 就断定最终进程能否生成代码。

华为当前文档区分：

- JSVM JIT：向 AGC 申请受限 ACL `ALLOW_EXECUTABLE_FORT_MEMORY`，更新签名 profile 后重新打包。
- 第三方引擎可写代码内存：`ALLOW_WRITABLE_CODE_MEMORY`，公开分发说明有设备类型限制。
- 坚盾守护模式另有限制；不能把 JSVM 的授权等同于允许 Chromium V8 任意生成可执行页。

## Lumie 探针为何要重启

`ohos_chrome_main_runner.cc` 中 `ProcessCanJit()` 启动时执行：

```cpp
page = mmap(..., PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, ...);
ok = mprotect(page, page_size, PROT_READ | PROT_EXEC) == 0;
munmap(page, page_size);
```

失败后在启动参数中加入 `--jitless --wasm-jitless`。
`aboutInfo.jitEnabled` 根据启动参数是否含 `--jitless` 返回，既不是实时系统开关查询，
也没有再次执行 mmap/mprotect。探针没有实际执行生成的机器码，也未验证全部 V8 分配路径。

因此，用户观察到同一包重启后的 0→1 与“新进程可执行内存环境发生变化”相容。
目前只能将微信启动视为相关触发事件；尚未区分内核共享状态、延迟初始化、
appspawn/prefork 权限传递，以及微信 XWeb 子进程启动引发的变化。
这些是当时待检验的假设，不是已经定位的根因；文末同进程实验已排除仅新进程继承的解释。

## 已知检查边界

- 手机使用 HongMeng Kernel，不能把 OpenHarmony Linux XPM 驱动实现直接当成实机行为。
- `/proc/sys/kernel/jitfort/jitfort_mode` 与 `/proc/sys/kernel/xpm/xpm_mode` 存在，
  普通 shell 和正常 HDC 读取均被拒绝，未得到值。
- 微信安装包与微信/appspawn 的内存映射也被拒绝读取，未取得微信 XWeb 二进制。
- 当前 CODE_SIGN 日志多次出现 `Ioctl cmd 40087803 failed: Invalid argument (ignore)`。
  它说明该 ioctl 在部分进程启动时失败，不足以证明全局开关状态或定位跨应用变化原因。
- 静态调查阶段未更改功能代码；后续仅在 Lumie 增加显式调试诊断入口。
  未申请新权限，未修改系统安全配置；两版 Chromium 的功能实现仍未改动。

## 冷启动对照方案（当天随后获准执行，结果见文末）

重启后先确认微信及 XWeb/分身进程是否已自动启动；若已启动，该轮不能作为干净的前置对照。
记录 boot 时间、appspawn PID、Lumie PID/PPID、探针 mmap/mprotect 返回值和 errno、启动参数。
先启动 Lumie取基线；启动微信后，在相同应用包、签名与设置下杀掉重启 Lumie复测。
分别记录微信主进程与 XWeb 子进程出现时点，避免把两者混为一个事件。
再用独立普通测试应用做同等可执行内存检查，才能评估“全局”的范围。

## 参考与本地证据

- [华为 JSVM JIT 权限指导](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/jsvm-apply-jit-profile)
- [OpenHarmony ownerid_utils.cpp](https://github.com/openharmony/security_code_signature/blob/master/interfaces/inner_api/code_sign_attr_utils/src/ownerid_utils.cpp)
- [OpenHarmony code_sign_attr_utils.c](https://github.com/openharmony/security_code_signature/blob/master/interfaces/inner_api/code_sign_attr_utils/src/code_sign_attr_utils.c)
- [OpenHarmony appspawn_common.c](https://github.com/openharmony/startup_appspawn/blob/master/modules/common/appspawn_common.c)
- [OpenHarmony app_spawn_client.cpp](https://github.com/openharmony/ability_ability_runtime/blob/master/services/appmgr/src/app_spawn_client.cpp)
- [OpenHarmony app_mgr_service_inner.cpp](https://github.com/openharmony/ability_ability_runtime/blob/master/services/appmgr/src/app_mgr_service_inner.cpp)

公开源码为 2026-10-26 获取的 master，仅作解释接口和数据来源的参考；名单成员来自实机二进制。
例如公开最新源码与实机在 enableJitFort 分支上已有差异，应以实机反汇编为准。
调查产物保存在本机 `/tmp/lumie-jit-investigation/`：`evidence.json`、系统二进制、
`appspawn-common.disasm`、`appspawn-common.annotated.txt`、公开源码副本及系统日志。
未将微信安装信息原始转储或系统二进制加入仓库。


## 继续向内核层调查（2026-10-26）

已直接读取系统版本参数，当前手机版本为
`PLA-AL10 7.0.0.109(SP6C00E105R4P3)`，`const.ohos.fullname` 为
`OpenHarmony-7.0.0.105`；`uname` 仍报告 HongMeng Kernel 1.13.0。
版本参数包含 OpenHarmony 字样不能证明实机使用公开 OpenHarmony Linux 内核实现。

### 内核镜像取得情况

正常 shell/HDC 权限尝试的结果：

| 入口 | 结果 |
| --- | --- |
| `/proc/kallsyms` | shell 查询和 HDC 读取均 Permission denied |
| `/sys/kernel/notes` | shell 查询和 HDC 读取均 Permission denied |
| `/proc/version`、`/proc/partitions` | Permission denied |
| `/proc/kcore`、`/proc/config.gz`、`/sys/kernel/btf/vmlinux` | No such file or directory |
| `/dev/block/by-name`、`/dev/block/platform` | Permission denied |
| by-name 下尝试的 kernel、kernel_a、boot、boot_a、ramdisk 路径 | Permission denied；不能据此确认这些分区名实际存在 |
| `/data/update`、`/data/updater`、`/patch_hw` | Permission denied |

在本机 Downloads、项目与相关诊断产物目录未找到匹配的内核镜像/完整升级包。
搜索精确型号/系统版本后未取得可验证的对应固件下载；搜索结果不等于证明公开渠道一定没有。
用户尚未提供完整 OTA 或镜像。目前无法开始实机内核本体的反汇编，限制在于缺少二进制输入。
该只读调查阶段未重启设备，未变更 bootloader、系统分区或安全设置；后续获准重启的对照见文末。

### 已继续反汇编到 libc 系统调用边界

成功读取实机 `/lib/ld-musl-aarch64.so.1`。

- SHA-256：`fb89216a4c2784389b16e941797318be275878aa46fb407a0543775d1c784bd0`。
- Build ID：`328898e259673984858815d01ecdb9e3`。
- `mprotect` ELF 虚拟地址：`0x19ef7c`。
- 普通分支 `0x19efcc` 将 226 放入 `w8`，`0x19efd8` 执行 `svc #0`；
  后续把负的系统调用错误转换成 errno 和 -1。
- `0x19efb4–0x19efc4` 会检查可选 hook 表；有 hook 时从 `0x19eff4`
  传递调用号 226 与参数，`0x19f004` 间接调用 hook。
- 库导出 `set_syscall_hooks`（`0x1c1c8c`），可更新该表及回调地址；
  尚未读取 Lumie 实例的对应内存，不能断言它实际走哪条分支。
- `mmap` 位于 `0x1feecc`，还有内存工具 hook 路径，普通路径进入 `mmap64`（`0x19ee18`）。

这一步确认了可执行内存申请在用户空间至系统调用边界的代码路径；
它不揭示内核内部 mmap/mprotect/XPM 的权限判断，也不能证明全局 JIT 状态变化。
反汇编保存在 `/tmp/lumie-jit-investigation/libc-memory-syscalls.disasm` 和 `mmap.disasm`。

### 拿到镜像后要追的内容

先验证型号、版本和镜像哈希，识别启动包封装、压缩/加密与内核架构。
能够提取有效内核代码后，再定位 mmap/mprotect 系统调用处理、XPM ioctl 处理、
进程类型 10 的执行权限分支、JIT 全局初始化变量及进程创建/继承逻辑。
若固件只有差分数据或镜像无法解密，仍需完整基线/可解析镜像，不能仅凭文件名声称已经分析。


## DevEco 模拟器 ROM 提取与对照（2026-10-26）

本机已下载的 phone_all_arm 镜像可直接离线读取，无需启动模拟器。
原始目录：`<user-home>/Library/Huawei/Sdk/system-image/HarmonyOS-7.0.0/phone_all_arm/`。
SDK 版本为 HarmonyOS 7.0.0.107(SP8)、API 26；内核是 **Linux 5.10.210 / AArch64**，
构建时间 2026-09-12，不能等同于实机 HongMeng Kernel 1.13.0。

提取产物及说明保存到：
`<local-evidence-root>/emulator-7.0.0.107-phone-arm64/README.md`。
包含原始 Image/ramdisk、内核配置、重建 ELF、约 12.6 万条内核符号、
appspawn/libc、init/param/SELinux 文件、相关反汇编及分区完整文件索引。
system/vendor/sys_prod 原始镜像仍位于 SDK 目录，未重复复制整个分区或读取实例用户数据。

内核 Image SHA-256：
`166788ee0de178e21ad77fbed9b4bc17f25b86b897f8c7b9f0abb0f17ad97906`。
其他源文件哈希见产物目录 `manifest.json`。

### 重建符号与已定位检查链

`vmlinux-to-elf 1.4.0` 重建的 ELF 不是原始调试 ELF。
工具有相对地址启发式警告，已独立核对版本字符串偏移、四个相关函数的原始机器码、
mprotect 的 BL 目标，以及 syscall 226 表项的重定位目标，均一致。
核验记录为 `symbol-validation.json`。
重建 ELF 的 syscall 表项仍需结合 `.rela.dyn` 解读，不能假设数据指针已全部重定位；
没有恢复源代码、类型或完整 DWARF 信息。

已定位以下静态路径：

- `__arm64_sys_mprotect`：`0xffffffc0101e9600`。
- `security_file_mprotect`：`0xffffffc0104be7e0`，遍历 LSM hook。
- `selinux_file_mprotect`：`0xffffffc0104ca7a0`。
- `file_map_prot_check`：`0xffffffc0104d0b40`，相关匿名可执行内存分支进入 SELinux 权限检查。

提取的 `system_common.cil:18766` 含
`(allow hap_domain self (process (execmem)))`。
未启动模拟器验证实际加载策略或应用 JIT 能力，不能把离线规则当成运行时测试结果。

### 与实机 XPM 路径的差异

- 内核配置与恢复符号未检出 XPM/JITFORT 对应实现；原始镜像中的策略类名不能证明驱动存在。
- 递归遍历 system 8,877 条目、vendor 448 条目、sys_prod 3,084 条目，
  未找到 `.ko` 或 `.ko.*` 文件；没有发现这些分区另行携带相关内核模块的证据。
- 模拟器 appspawn common 的符号/字符串未检出实机的
  `InitXpmWithParam`、`ConvertIdType`、`/dev/xpm` 和微信 appIdentifier，
  但仍有 `persist.security.jitfort.disabled`。名称缺失不单独证明等效逻辑绝对不存在。
- SELinux/参数文件保留 XPM、JITFORT、代码签名声明，不能仅凭这些声明推导内核实现。
- 另提取 JSVM 与 Ark 的运行时名单；它们不能等同于实机 XPM 签名身份放行名单。

模拟器 appspawn common SHA-256：
`7110319897666409d6a20780ff685fa806a80c60929e477883571e7abb4db827`；
Build ID：`d8ef54de387f4306985ad5dce7723cf2`。

这些文件可供继续反汇编和接口对照，但未定位实机跨应用 JIT 变化的根因，
也未证明“微信开启系统全局 JIT”。本次没有启动模拟器或重启手机，没有修改系统安全设置。

### 模拟器 Linux 可执行内存实现补充

进一步反汇编确认，syscall 226 先检查当前地址空间 VMA 的权限上限，再进入 LSM/SELinux；
通过后调用 mprotect_fixup → vma_set_page_prot → change_protection，
更新目标映射、页表与 TLB，并包含指令/数据缓存同步分支。
一次调用只改变对应进程地址空间的指定区间，不授予其他独立进程执行权限。

匿名可执行内存路径查询当前 SID 的 process/execmem。
提取的策略明确允许 hap_domain 的 execmem，且该集合同时含 normal_hap、debug_hap；
这只是离线策略证据，并非对全部 JIT 路径的运行验证。

default_noexec 位于内核 ro_after_init 区间，并在 selinux_init 中赋值，
不能将 Image 中的零初值当作运行时状态或应用可切换的全局 JIT 开关。
avc_denied 确实读取全局 enforcing 与域 permissive 属性，但未发现微信改动这些状态的证据。
正常 AVC 缓存复用策略决定，不会因其他应用调用成功而新增授权。

新增相关函数机器码与原始 Image 的比对全部通过。
详细流程、地址及公开 v5.10.210 源码交叉说明见产物目录的 `JIT_IMPLEMENTATION.md`；
仍不把模拟器实现等同于 Mate 70 的 HongMeng Kernel。

## 实机重启对照结果（2026-10-26 17:16–17:35）

用户本轮授权重启。共正常重启手机两次；第一轮解锁时微信主进程自动启动，
早于 Lumie 的有效采样，不能作为主微信未启动基线。第二轮取得如下序列：

| 第二轮阶段 | Lumie PID | 原生探针 | config jitless |
| --- | --- | --- | --- |
| 首次启动；仅微信通知进程存在 | 16616 | unavailable（0） | 0 |
| 不打开主微信，再杀掉重启 Lumie | 19012 | unavailable（0） | 0 |
| 用户打开主微信后，杀掉重启 Lumie | 24310 | available（1） | 0 |
| 强制停止微信后，再杀掉重启 Lumie | 27001 | available（1） | 0 |

四条原生探针日志的启动时间分别为 76.452、140.724、312.263、376.445 秒。
系统 appspawn 始终为 PID 572。Lumie 为同一已安装 1.0.1 debug 包，
Chromium 155.0.8059.91 / 7be534b5；全程没有重新安装或调整 JIT 配置。

微信通知进程 PID 14598 在 67.694 秒已记录
`Xpm: app in temporary allow list`，但随后两次 Lumie仍为 0。
主微信 PID 24199 出现后，287.138 秒记录相同名单日志；
其后 Lumie 新进程才测得 1。截至四次对照完成的采样未发现 XWeb/分身/nativespawn，
但约 2 秒采样间隔不能排除极短暂或使用其他名称的进程。
对照完成后 17:32:44 才首次采到 XWeb PID 30164、30223 及 nativespawn PID 30188，
晚于首次变为 1 和停止微信后的复测；不能笼统说整个第二轮都没有 XWeb。

停止微信后，采样显示微信主/通知进程均已退出，Lumie重新启动并取得 1。
通知进程稍后被系统自动拉起，新的名单日志为 382.481 秒，晚于该次探针。
因此结果不要求微信主进程持续存活，但尚不能区分内核状态、appspawn 状态或其他系统活动。

需要保留的限制：

- 没有取得完全隔离系统后台活动的环境，也未用其他普通 App 验证“全局”范围。
- getenforce、坚盾状态、jitfort 参数读取仍被拒绝，没有确认具体开关值。
- 原生探针只检测 mmap(RW) → mprotect(RX)，没有 errno 或机器码执行结果。
- ioctl 40087803 的 Invalid argument 在 0/1 两种结果下都出现，不能据此判断开关。
- aa start 微信曾返回成功但主进程未出现；以实际进程和日志为准，最后由用户手动打开主微信。
- 为取得探针日志临时设置 AuraShell 日志级别 I，没有改变安全参数、权限或签名。

17:35 左右关于页截图明确显示“JavaScript JIT：已开启（1）”，版本和提交与上述样本一致。
完整报告、四次原始日志、进程时间线与截图保存在：
`<local-evidence-root>/phone-reboot-20261026-171656/`。
入口为 `README.md`，结构化结果为 `probe-summary.json`，截图为 `54-jit-enabled.png`。
采集已停止，原始设备记录未加入仓库。当前已复现用户报告，具体根因仍待进一步追踪。


## 同进程定位及自主初始化实验（2026-10-26）

详细原始记录、调试包和静态样本位于：
`<local-evidence-root>/phone-samepid-20261026-175625`。


保留同一个 Lumie PID 15787、PPID 571，分别通过 libc 和 AArch64 内联 `svc`
执行匿名页 RW→RX。后者不经过 libc 的可选 syscall hooks。

| 阶段 | PID | 样本序号 | libc mprotect | 直接 mprotect 系统调用 |
| --- | --- | --- | --- | --- |
| 微信前 | 15787 | 1 | -1 / errno 22 | -22 |
| 微信前复测 | 15787 | 2 | -1 / errno 22 | -22 |
| 打开微信后，Lumie 未重启 | 15787 | 3 | 0 | 0 |
| 停止微信后，Lumie 未重启 | 15787 | 4 | 0 | 0 |

每次 mmap 和 munmap 均成功；地址经过一次实际写入后再改 RX。
微信主进程 PID 21269 首次采样于 18:01:55.630，XWeb 子进程首次采样于
18:01:59.049，成功样本采于 18:02:09。此轮无法区分主进程与 XWeb 的触发作用。
此前另一轮 0→1 对照的采样中，XWeb 出现得更晚；两轮不能混用时间线。
18:02:58 停止微信后，18:03:00 的进程快照已无微信进程，Lumie 仍成功。

结论：变化可以影响已经存活的进程，且确实影响直接系统调用的结果。
因此可以排除“只给新建进程换配置”以及“只有 libc 包装层放行”作为完整解释。
证据支持跨应用影响到内核侧可执行内存判定的状态变化；还没有定位具体内核变量、
服务或微信调用，也不能外推成所有 UID/应用均被全局授权。

Lumie 原有引擎仅在启动时探测一次，失败后固定加入 `--jitless --wasm-jitless`。
因此旧进程的内存能力已经变化，关于页与已启动 V8 仍可保持 0；这解释了用户需要重启
Lumie 才看见 1，而不意味着内核必须在 Lumie 重启后才放行。

## 系统 JIT 接口反汇编与交叉佐证

- 手机 `/system/lib64/ndk/libjsvm.so` 的 0x337bc–0x337cc 调用
  `prctl(0x6a6974, 69, 0)`；公开 `arkcompiler_jsvm` 源码标注 69 为 `'E'`，
  名称 `JITFORT_QUERY_ENCAPS`，`HasJitfortACL()` 以返回 0 作为通过。
- JSVM 还查询 `AdvancedSecurityModeGetStateByFeature("RESTRICTED_JSVM_FEATURES", ...)`；
  这是读取安全模式状态，不是开启接口。本实验没有调用任何安全模式设置接口。
- 手机 `/system/lib64/platformsdk/libark_jsruntime.so` 的 0x9d631c 函数在
  0x9d636c、0x9d6384 两次调用 `prctl(0x6a6974, 5, 0)`。
  与公开 `JitFort::InitJitFortResource()` 对应：首次成功、再次 EEXIST。
- 二进制哈希与源码版本见 `jsvm-static-evidence.json`、`ark-jitfort-static-evidence.json`。

所有同进程样本的 ACL 查询均为 libc -1/errno22、直接调用 -22。
只能说明该查询没有返回通过，不能区分不支持此查询与策略拒绝，亦不能据此证明
所有内部权限字段未改变。

## Lumie 自己请求 JITFORT 的实测

在微信已使普通 RX 映射可用的状态下，PID 28170 请求初始化：首次返回0，
再次 errno17（EEXIST）。随后又重启整机，在没有观察到微信或 XWeb 进程的情况下测试：

- PID 8670 的样本1、2、3，普通 RW→RX 均返回 EINVAL。
- 同一个进程请求 `prctl(0x6a6974, 5, 0)`，返回0，再次 EEXIST。
- 紧接着的样本4、5，普通 RW→RX 仍返回 EINVAL；libc 和直接系统调用一致。

因此该请求可以初始化本进程的 JITFORT 资源，但不足以开启 Chromium 当前所需的
普通匿名页 RW→RX 路径。没有证明它能替代微信触发的状态变化，也没有得到可用的
自主开启方案。初始化成功不能标成“JIT已开启”。

## 诊断代码和验证状态

共享 shell 原生模块新增 `jit_probe.c`，通过 debug 构建的显式 Want 参数触发。
正常启动不执行；不会自动修改引擎开关。关于页仍报告引擎启动时的配置。
诊断不执行生成代码，所以本报告的“可用”仅指测得的内存权限操作成功。

编译完成并覆盖安装，保留应用数据；devecocli run 的 smoke 通过。
ArkTS 独立检查器不能解析生成的 BuildProfile 模块（原有页面与新增引用均报同类错误）；
完整 Hvigor 构建通过。无提交、推送或新权限声明。

本阶段末已编译标准映射变体，因 HDC 断开暂未执行；随后重连完成，结果见文末。

## 证据文件

- `samepid-probe-summary.json`：同 PID 的四个已核对样本。
- `self-init-probe-summary.json`：冷启动初始化前后样本与调用结果。
- `06-process-timeline.jsonl`：每约1秒采样；不能排除间隔内极短命进程。
- `13-after-stop-samepid-probe.json`、`24-after-self-init-probe.json`：原始日志。
- `probe-signed.hap` / `probe-init-signed.hap` 及各自哈希：实际使用的调试包。

进一步定位到内核函数/变量仍需要这台手机的 HongMeng 内核映像或对应符号/跟踪能力，
以及微信侧触发调用的证据。DevEco 模拟器的 Linux 5.10.210 不包含这套实机实现，
不能用它替代手机内核得出确定根因。


## 再次重启及标准映射完整对照（2026-10-26 18:50–19:00）

详细记录：`<local-evidence-root>/phone-mappings-20261026-185018`。


18:56:03 第二次重启后，Lumie PID 9025、PPID 570，首次启动入口立即执行探针。
在主动启动微信之前，约1秒间隔的进程采样未观察到微信或 XWeb。

| 操作 | 冷启动 | 自行初始化 JITFORT 后 | 打开微信后，同 PID |
| --- | --- | --- | --- |
| 匿名 RW→RX（libc / 直接 svc） | EINVAL(22) | EINVAL(22) | 成功 / 成功 |
| 匿名 RWX→RX | mmap 阶段 EINVAL(22) | 同左 | 成功 |
| 匿名 RX→RW→RX | mmap 阶段 EINVAL(22) | 同左 | 成功 |
| 匿名 NONE→RW→RX | 添加 EXEC 时 EINVAL(22) | 同左 | 成功 |
| 匿名 NONE→RX | 添加 EXEC 时 EINVAL(22) | 同左 | 成功 |
| 匿名 RW→R→RX | 添加 EXEC 时 EINVAL(22) | 同左 | 成功 |
| 写入 memfd 后映射 RX | EACCES(13) | EACCES(13) | EACCES(13) |

JITFORT 请求仍是 `prctl(0x6a6974, 5, 0)`：首次成功，重复 EEXIST(17)。
这与普通匿名页执行权限是否可用是两回事。ACL 查询子操作 `'E'` 始终返回 EINVAL。

18:58:50 用 `aa start ... --pi ohos.extra.param.key.appCloneIndex 0` 指定原版微信，
这次直接启动了主进程，避免此前的分身选择界面。微信主进程 PID 20167 首次采于
18:58:50.946，XWeb PID 21106/21175 首次采于18:58:54.270；18:59:15 复测成功。
本轮不能区分微信主进程和 XWeb 中哪个具体调用触发了变化。

19:00:02 停止微信后，PID 9025 的 libc/直接 RW→RX 仍成功，快照无微信进程。
随后重启 Lumie，新 PID 21940 的 RW→RX 也成功，引擎日志为
`AuraShell JIT available (config jitless=0)`。旧 PID 的引擎启动日志则为 unavailable。

## 结论与边界

再现了影响存活进程的匿名执行内存状态变化。标准分配顺序、直接 RX/RWX 分配、
memfd 以及 JITFORT 子操作5初始化，都未能独立开启冷启动时的普通匿名可执行页。
memfd 始终失败，不能将现象表述为“所有执行内存限制都已解除”。

尚未得到实机内核映像或具体触发调用证据，不能指定内核变量或断言微信使用某个 prctl。
本组探针未执行生成代码；成功表示测得的内存操作通过，不等于验证了完整 V8 行为。
进程采样不能排除间隔内极短命的进程。

## 排除的第一轮与诊断调整

18:51:27 的第一轮重启中，启动早期 INFO 日志未捕获到基线；18:53:27 微信通知与
主进程已在后台出现，之后测得成功。用户报告未手动打开微信，不能将此轮当作干净冷启动。
于是将显式调试探针改用 WARN 日志，并在首次 onCreate 一次执行整组操作，再进行了
上述第二轮有效对照。所有入口仍要求 debug 构建与显式 Want 参数，正常启动不执行。

## 证据

- `validated-summary.json`：冷启动、子操作5初始化、微信后、停止微信后，逐项断言核对。
- `new-process-summary.json`：最终新 Lumie 进程结果。
- `19-early-coldboot-probe-*.json`：首次启动的原始整组日志。
- `21-cold-after-self-init-probe.json`、`25-after-wechat-probe.json`：同 PID 原始日志。
- `08-process-timeline.jsonl`、`second-boot-wechat-first-seen.json`：进程时序。
- `29-new-lumie-engine.json`：引擎启动配置。
- `early-probe-signed.hap`、`early-probe-sha256.txt`、`early-jit_probe.c`：实测调试包与源码。

完整构建与覆盖安装 smoke 均通过，保留应用数据。最后已停止进程采样。



## VintagePomeloPro 代码核对与子操作0实测


用户提供仓库 `yifengling0/VintagePomeloPro`，读取提交
`5383b7849190c563c0e79007fb9c33da64b326e4`。
其 Wine gitlink 指向 `winehua/wine@1cb1ac93e464fe6db019845f3451f50ef6fc78f4`。

- [ohos_virtual.h 第34行](https://github.com/winehua/wine/blob/1cb1ac93e464fe6db019845f3451f50ef6fc78f4/dlls/ntdll/unix/ohos_virtual.h#L34)：
  `ohos_jit_enable()` 调用 `prctl(0x6a6974, 0, 0)`；对应 disable helper 的第三参数是1。
- [ohos_virtual.c 第151行](https://github.com/winehua/wine/blob/1cb1ac93e464fe6db019845f3451f50ef6fc78f4/dlls/ntdll/unix/ohos_virtual.c#L151)：
  `ohos_mprotect_exec()` 先 enable，再 mprotect，当前实现不调用 disable；注释称 Box64 后续仍需要。
- `virtual.c:1967` 在 OHOS 的执行页路径调用该 helper。
- Box64 gitlink `0411b38569d3b70a0a8eb64ffd80e7ef72f9cb67`，
  `src/wrapped/wrappedlibc.c:4552` 针对0x6a6974转发并将最后两个参数清零。

父仓库 `docs/NOEXEC_MMAP_ANALYSIS.md` 的旧片段显示成对关闭，不能替代上述当前实现。
`docs/OHOS_MMAP_ANALYSIS.md` 描述 HAD-W32/Linux 和额外权限的历史测试环境，
而当前 module.json5 仅声明 ALLOW_WRITABLE_CODE_MEMORY；因此不能直接将旧文档结论套到本机。

## 对照结果

19:10:12 重启整机。初始无 clone index 的启动请求虽返回成功，却没有目标进程或探针日志。
随后明确指定 `ohos.extra.param.key.appCloneIndex=0`，启动原版 Lumie，PID18734，PPID571。
先执行普通探针，保留不可用基线，再在同一进程显式请求子操作0。

| 阶段 | PID | 结果 |
| --- | --- | --- |
| 原始基线，seq1/2 | 18734 | libc mprotect=-1/errno22；直接 svc=-22 |
| 紧邻请求前，seq3 | 18734 | 两条路径仍为 EINVAL(22) |
| `prctl(0x6a6974, 0UL, 0UL, 0UL, 0UL)` | 18734 | rc=0，errno=0 |
| 紧邻请求后，seq4 | 18734 | 两条 mprotect 路径均成功 |
| 五种标准匿名映射变体 | 18734 | 全部成功 |
| memfd 写入后映射RX | 18734 | 仍为 EACCES(13) |
| 结束原进程，重新启动，不再调用子操作0 | 22132 | libc/直接 RW→RX 均成功 |

seq3 的单调时间184892.503ms，seq4为184893.270ms；调用前后相隔不足1ms。
整轮约1秒间隔进程采样未观察到微信或 XWeb。
JSVM ACL 查询子操作'E'始终 EINVAL，没有变成通过。
新 PID22132 的引擎启动日志为 `AuraShell JIT available (config jitless=0)`；
原 PID18734 的启动日志为 unavailable，因为 V8 在显式请求之前已经固定 jitless。

## 可以得出的结论

在本机、本签名及现有权限条件下，子操作0是已验证有效的自主开启入口，
与此前仅初始化资源、未能放行普通匿名页的子操作5不同。
效果影响匿名执行内存，并且保留到结束请求进程后新建的同应用进程。
这与共享状态的解释相容，但尚未区分按UID、按应用或更大范围的状态，不能称所有应用均被授权。

尚未证明微信调用了相同入口，也未取得实机内核函数/变量的反编译证据。
探针没有执行生成机器码，完整 V8 所有分配/执行路径及其他系统版本仍未覆盖。
没有调用 disable helper；没有通过写 sysctl、更换签名或伪装微信标识实现。

## 交付和证据

已编译并覆盖安装显式调试入口 `lumieJitCompat`，保留数据。正常启动不自动调用。
完整构建和安装 smoke 通过；独立 ArkTS 检查器仍不能解析生成的 BuildProfile，完整构建可解析。

- `validated-compat-summary.json`：经断言核对的请求前后原始日志事件。
- `10-after-compat-probe.json`：系统日志原文。
- `13-new-plain-result-probe.json` / `13-new-plain-result-engine.json`：新进程及引擎日志。
- `05-process-timeline.jsonl`：进程采样；不能排除采样间隔内极短命进程。
- `reference-source/`：固定 gitlink 提交的 Wine/Box64 源文件。
- `compat-probe-signed.hap`、`compat-probe-sha256.txt`、`jit_probe.c`：实测调试包及源码。

只有签名相同的现有 Lumie 条件经过验证；不要把这条私有 ABI 当成跨版本稳定的公开授权API。
