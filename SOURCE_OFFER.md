# Corresponding Source

This branch provides the reusable Chromium/HarmonyOS engine source mapping for
the locally validated downstream test HAP with SHA-256:

`babb4439520275fbe34573b6537c7afb237ac785af06ec3dcd35dd424d9b0d53`

The corresponding source consists of:

1. Chromium revision `f405107495a07cb1bfcf687d4af8d91117098db6` and the
   third-party revisions pinned by its DEPS file.
2. `patches/chromium-154-harmonyos.patch`.
3. The complete `overlay/` and `external/` trees in this branch.
4. `config/args.plan_kirin_pc.gn` and the build instructions in `BUILDING.md`.
5. `SOURCE_MANIFEST.sha256`, which verifies every published regular file other
   than the manifest itself.

The public tree intentionally uses the generic Chromium HarmonyOS Adapter
identity. Downstream application names, artwork, signing certificates,
provisioning profiles, and private signing configuration are not included.
Distributors remain responsible for providing any additional corresponding
source required by the licenses that apply to their downstream changes.

For at least three years from 2026-08-10, recipients of that binary may request
an electronic copy of these materials through this repository's GitHub issues.
The source is provided without charge. System SDKs and standard toolchain
components are not included; obtain them from their respective vendors.
