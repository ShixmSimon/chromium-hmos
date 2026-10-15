# Brave execution integration — acceptance in progress

Target: Chromium 154.0.8037.97; adblock-rust 0.13.3; Rust 1.96.1.
The prior native package is 55345175. This change is not yet device-accepted.

The pinned Brave core Cargo.lock resolves adblock 0.13.3, matching the existing
vendored crate and adblock-resources package. Exact resource pins, hashes and
licensing are in overlay/third_party/adblock_rust/resources/SOURCES.json.
The component updater does not provide a core-wide lock for resources/lists:
this is a reproducible Lumie combination, not a claimed Brave release clone.

The C ABI is version 2; the loader rejects an incompatible DSO. Handles and
strings remain Rust-owned, immutable snapshots are Send+Sync, panic boundaries
remain in compile/query. Input is bounded at 65 MiB, output at 8 MiB. Failed
compilation/resource validation retains the prior snapshot. Resources are
embedded offline, with full original source and licenses included in the HAR.

| Capability | Implementation | Automated evidence | Native device |
|---|---|---|---|
| Network / cross-list exceptions / important | shared immutable engine; browser and renderer throttles | Rust regression | Pending |
| redirect / redirect-rule | local URLLoaderFactory substitution, MIME, taint, no-store, same request origin | Rust resource/decision semantics | Pending original-request absence, CORS, actual media/script consumption |
| removeparam | async deferred same-origin URL override, redirects, eight-rewrite bound | Rust parameter/encoding semantics | Pending server-observed URL |
| CSP | additional raw and parsed enforcing policy; original policies retained | Rust directive/exception semantics | Pending native response merging |
| scriptlets | synchronous document-start payload; native main-world execution | Host Chrome first-inline, CSP and Trusted Types controls | Pending Chromium154 timing, cross-origin frames |
| Cosmetic / procedural / actions | Brave selector evaluator, isolated scheduler, native user styles | Host Chrome all parser operator classes and actions, dynamic/disable | Pending BFCache, prerender, high-load phone performance |
| Worker requests | renderer tokenless requests and browser worker script bootstraps; worker-origin exception policy | Rust resource types | Pending dedicated/shared/service worker actual requests |
| WebSocket | browser connector gate before the network service handshake | Rust websocket matching | Pending server handshake evidence |
| Conditions | bounded if/else/endif, !/&&/||/parentheses; unknown conditions exclude both branches | Rust preprocessing | Pending list compatibility |
| include | explicitly unsupported, counted; no extra fetch/local file expansion | Parser boundary | Not implemented by design |

The ordinary v1 commands/events/context/revision/source IDs remain unchanged.
Existing EasyList/China defaults stay on; EasyPrivacy stays off. New optional
`brave-compat` pairs Brave-specific with Brave-unbreak, defaults off, updates
only with the signed package. It includes exceptions which assume other Brave
protections, so is not silently enabled. On already-full 32-source profiles it
is omitted rather than evicting a saved subscription. No Android/iOS lists or
Brave service credentials/component IDs are imported.

Global/site toggles stop removable cosmetic effects and observers. Scriptlet
side effects and removed DOM require a user refresh; no automatic refresh or
restart occurs. Shared/background workers use their validated worker origin
for site exceptions and have no tab statistics. A frame-bound request uses the
top site. Internal/extension origins and browser subscription factories remain
excluded. Normal/OTR services keep the original v1 separation.

Local test commands:

```
bash scripts/test-adblock-rust.sh
CARGO_TARGET_DIR=/tmp/lumie-adblock-tests cargo +1.96.1 build --locked \
  --manifest-path overlay/third_party/adblock_rust/Cargo.toml --example probe
PLAYWRIGHT_MODULE=/path/to/playwright/index.mjs \
ADBLOCK_PROBE=/tmp/lumie-adblock-tests/debug/examples/probe \
CHROME_PATH=/path/to/chrome node scripts/tests/adblock-browser.mjs
```

Host Chrome tests exercise shipped Rust payloads and executor JS. CDP installs
main-world startup scripts and styles for this test; it does not execute the
OHOS Mojo/URLLoader integration and must not be reported as native acceptance.
Network execution and native lifetime acceptance require the controlled device
probe. Performance numbers must identify host/device and debug/release builds;
no phone startup/load/scroll/memory improvement is claimed without measurement.

## Validation snapshot (2026-10-15)

- 19 Rust tests passed, including resource permissions, aliases, redirect-rule,
  CSP exceptions, removeparam encoding, and bounded condition preprocessing.
- 20 functional and 2 bounds/cancellation host Chrome 155.0.8059.40 tests passed against the shipped Rust ABI payload
  and procedural executor. These are not Chromium 154 native hook tests.
- Existing Lumie shell contract tests: 12 passed. No shell protocol changes.
- OHOS arm64 Rust DSO cross-compilation passed. Complete native HAR build is
  tracked separately; a cross-compiled Rust library alone is not a HAR.
- The controlled server was exercised on an unfiltered host Chrome. All
  expected filtering negative controls failed, while allowed requests, original
  CSP, worker endpoints and WebSocket handshakes succeeded. Server hit records
  independently confirmed that the unfiltered requests arrived.

Host-only release benchmark: macOS arm64, five alternating runs against
55345175, bundled EasyList + China, 12000 deterministic requests per run.
Both builds blocked 4000 requests. Medians across the five runs:

| Measurement | 55345175 | Candidate |
|---|---:|---:|
| Engine compile | 49.988 ms | 51.213 ms |
| Per-run query p50 | 1.500 µs | 1.917 µs |
| Per-run query p95 | 1.791 µs | 2.083 µs |
| Whole process peak RSS | 69,337,088 B | 75,005,952 B |

RSS includes the probe's retained JSON results, which are larger in ABI v2; it
is not an isolated measurement of engine heap. Resource loading and richer
results add cost. These figures do not predict phone startup, rendering or
scrolling performance; those measurements remain pending.

## Explicit boundaries

- The v1 capability array remains `network`, `cosmetic`. No new capability is
  advertised as accepted before native runtime validation. The new execution
  paths are compiled in and do not require an additional shell switch.
- Redirect replacement supports GET/HEAD, including document/frame resources.
  Navigation responses use basic response tainting and parsed response headers;
  URLs/origins stay with the requested resource. Only the fixed resource bundle
  supplies content (`noop.html` / `noopframe` is the bundled HTML replacement).
  Native frame/main-document replacement and origin checks remain pending.
- No arbitrary downloaded JavaScript is executed. Custom sources cannot invoke
  trusted resources. Only the pinned offline `brave-compat` pair has the
  reviewed trusted permission mask; its downloaded-text form cannot inherit it.
- `!#include` is unsupported and reported. Unknown conditions exclude both
  branches, including negated unknown conditions. Platform identity flags are
  not falsely enabled to force a list's rules to match.
- Procedural work yields between selector calls after 4 ms and batches at most
  256 element operations per task. One synchronous selector call cannot be
  preempted. Limits are 1024 procedural rules and 20000 retained matches per
  document; excess matches/rules are not applied. Large-page behavior still
  requires native performance testing.
- BFCache/prerender, cross-origin frame execution, renderer restart, response
  CORS, cached/SW-intercepted requests and all three process modes remain native
  acceptance items. No claim of full Brave behavioral equivalence is made.

## 用户自行真机验收

使用新 HAR 替换外壳依赖、重新安装后，先检查 `getAboutInfo.engineCommit`
与交付提交一致；不要只看包名。保存现有个人规则，使用受控规则测试时暂时关闭
其他订阅，确保不是旧规则碰巧产生相同结果。

在电脑上运行：

```sh
python3 docs/tools/adblock_brave_probe.py --bind 127.0.0.1 --port 8109
```

另一个终端由用户建立 USB 反向转发（hdc help 已核对语法）：

```sh
/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/toolchains/hdc rport tcp:8109 tcp:8109
```

手机打开 `http://127.0.0.1:8109/`。localhost 是安全上下文，可覆盖 Service
Worker；局域网 HTTP 地址只能覆盖其余用例，不能把 SW 未执行算通过。
多个设备时，在 hdc 后加 `-t 设备序列号`。测试完成按 `hdc fport ls` 返回的
任务标识执行 `hdc fport rm 任务标识`，并停止本地服务器。

1. 复制页面显示的规则到广告拦截的个人规则，开启总开关及本站过滤，保存并手动刷新。
2. 点击“开始验证”，等结束后“导出结果”。检查页面结果和最后的服务端请求记录，
   尤其是替代资源可消费、原请求未抵达、实际 URL 参数已移除。另点击“测试主文档替代”，
   页面会短暂打开测试标签并关闭；完成后再导出。若新标签被阻止，该项需重测。
3. 关闭本站过滤、手动刷新再跑一次：过滤项应失败，允许请求及原站点 CSP 应继续通过。
   这轮是必要负例，并非修复回归。然后恢复本站过滤。
4. 手机默认单进程、实验多进程各跑一次；平板原生多进程单独记录。切换模式按外壳
   提示重启。记录内核提交、设备/API、模式、规则来源、JSON 与相关日志。
5. 再验收跨站前进/后退、同页导航、iframe、切后台恢复、多标签关闭、renderer
   恢复、普通/无痕与本站例外。此页面不替代这些生命周期用例。
6. 恢复原有规则和订阅。真实网站视频测试单独记录日期、登录状态、视频类型、
   播放/误伤表现；一次没有广告不能代替受控验收。

上述流程由用户操作，本轮没有安装或启动手机应用。

## Upstream comparison boundary

The parser emits CSS selector, has-text, matches-attr, matches-css,
matches-css-before/after, matches-path, min-text-length, upward and xpath
operators, plus style/remove/remove-attr/remove-class actions. The host suite
covers each of these classes. The imported Brave executor also contains helpers
that adblock 0.13.3 does not emit (for example matches-property); their presence
is not a claim that the corresponding rule syntax is supported.

A local Brave 1.97.57 / Chromium 155.0.8059.80 reference run was attempted in
fresh temporary profiles. Its custom-list UI reported download failure for the
local list, and a direct temporary custom-filter preference fixture did not
produce even the basic blocking positive control. It is therefore **not a valid
Brave behavioral comparison**, and is not used to claim compatibility or
upstream lack of support. A working matching Brave/resource reference run remains
pending. No existing Brave user profile was changed.

The additional `scripts/tests/adblock-stress.mjs` uses the same environment
variables as the browser suite. It verifies the 20000-match cap with 25000
elements, full retained-marker cleanup, and cancellation during a partially
applied batch (257 elements had been marked in the recorded run; zero remained
after stop). It does not establish a phone frame-time or scrolling budget.

Resource exclusions follow the pinned upstream combination: Brave's packager
`adBlockRustUtils.js` excludes `google-ima-dai.js`; adblock 0.13.3's
`resource_assembler.rs` excludes resources with `params` (the pinned uBlock
`click2load.html`). Neither is claimed as supported. These are upstream baseline
boundaries, not resources silently lost from the selected Brave combination.
Lumie's explicit trusted-resource permissions are intentionally stricter than
Brave's fully trusted personal-filter provider, as required by the shell handoff.
