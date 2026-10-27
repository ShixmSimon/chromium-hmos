# Chromium 155 migration

The stable branch is `hmos-154-adapter`; its persistent source and output stay
at `/root/chromium-154/src` and `out/ohos_arm64` inside that checkout.
`hmos-155-adapter` uses a separate `/root/chromium-155/src` and output directory.
The two jobs share the existing serial GitHub Actions concurrency group.
Only these two branches may run native builds, including manual dispatches.

The 155 pin is 155.0.8059.91, commit
`f043649a0ef1fd96e48d56ddbfc8a4a99aa2ca37` from the official Chromium mirror.
The 154 pin remains 154.0.8037.97. Branch-specific pins live in
`config/engines/`.

`patches/migration-155/manifest.json` records the complete tracked native
adaptation exported read-only from the successful `5834ccdf` build, including
all nine modified DEPS repositories. This includes subsequent incremental
patches absent from the original 154 base patch. Whole-file additions remain
in `overlay/`. The snapshots carry full base blob IDs for three-way migration;
hashes reject accidental partial edits. A conflict is a failed migration,
never permission to silently discard upstream or adapter changes.

Preparation is CI-only and push-triggered. It copies independent files and
Git objects from 154 without its `out/`, synchronizes the new pinned DEPS,
ports the snapshots, and runs the new toolchain hooks. SDK links may refer to
the same licensed SDK installation; source and writable build outputs are
separate. No native compiler is run from the interactive development session.

The 155 incremental build skips the legacy 154 incremental replay because
the complete snapshot already includes those changes. Future native edits
must update the migrated snapshot or overlay. The snapshot receipt avoids
reapplying identical patches on every build. When a pushed fix changes a
snapshot, identical input contents retain their timestamps after replay, so
already completed compilation is preserved.

Migration is complete only after patch/dependency migration, GN generation,
the full native build, runtime packaging and native rule-validation tests
pass. Phone/Pad runtime regression is recorded separately and must not be
inferred from a successful build.

The `ci/depot-155` trial also supports a fresh native build through
`.depot/workflows/native-build.yml`. From its published, clean checkout, run
`depot ci run --org <org-id> --workflow .depot/workflows/native-build.yml`.
The workflow checks out the requested SHA and verifies it against the
published trial branch; Depot's synthesized default branch ref is not used
to choose the engine.

Depot uses a 16-CPU Ubuntu 24.04 runner with Ninja `-j16` and the local
`/mnt/chromium-155` disk. It downloads the pinned 155 source, DEPS and tools,
then applies the same digest-checked migration snapshots as production.
It does not require a pre-existing 154 checkout. Source, SDK, tools and
completed objects are restored/saved with `actions/cache` under `depot155-*`
keys, isolated from 154. Completed APT downloads use a separate cache, and
OS packages are installed in every new sandbox. A prepared workspace is
saved even after a compile failure, but not after cancellation.

On the first cache miss, upload the private native target SDK subset to
the authenticated Depot sandbox and extract it at
`/mnt/chromium-155/ohos_sdk`. Write its verified archive SHA-256 to
`/mnt/chromium-155/sdk-inbox/sdk.ready` only after extraction is complete.
`config/depot-155-sdk.json` pins the SDK version, API and archive digest;
the archive and signing material must not be committed to Git. Later runs
restore this SDK from the isolated cache. The workflow uploads native
runtime, symbols and diagnostics as CI artifacts, without publishing a
release or packaging/signing a HAP.
