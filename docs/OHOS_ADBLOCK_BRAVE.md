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
