# Chromium 155 migration

The release branches are `hmos-154-adapter` and `hmos-155-adapter`. GitHub Actions
uses separate `/root/chromium-154/src` and `/root/chromium-155/src` checkouts on
the existing self-hosted runner. Both branches share the native concurrency
group, so compilation and runtime inspection do not overlap.

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

The self-hosted preparation copies independent files and Git objects from 154
without its `out/`, then synchronizes the pinned 155 dependencies and applies
the snapshots. Native compilation runs in CI. The separate Depot preparation
path remains available as archived tooling; see [DEPOT_CI.md](DEPOT_CI.md).

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

The active native workflow is `.github/workflows/incremental-build.yml`. It
selects the branch-specific checkout, prepares 155, builds with Ninja `-j12`,
runs native rule validation and packages the unsigned engine HAR. Push builds
publish the HAR, unstripped symbols and SHA256SUMS in a GitHub prerelease.
Manual dispatch supports a verification HAR without prerelease publication.
Private SDK and signing material must not be committed; HAP signing remains
on the developer machine.
