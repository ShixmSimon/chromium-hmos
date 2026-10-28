# Depot CI (archived)

On 2026-10-28 both release branches returned to GitHub Actions. The original
workflows were restored unchanged from `_bak/workflows/` to `.github/workflows/`.
Native builds use the existing `self-hosted, harmonyos-chromium` runner, with
separate `/root/chromium-154/src` and `/root/chromium-155/src` trees and shared
serialization. Adapter checks use GitHub-hosted runners.

Depot workflow files are retained under `_bak/depot-workflows/`; no active
`.depot/workflows/` remains. The following describes the archived Depot setup
and is not the current automatic CI path. Existing artifacts and caches remain
available; restoring GitHub Actions does not delete them.

## Engine and cache isolation

| Branch | Native workspace | Native cache prefix | Package cache prefix |
| --- | --- | --- | --- |
| `hmos-154-adapter` | `/mnt/chromium-154` | `depot154-linux-x64-v2-` | `depot154-apt-ubuntu24-x64-v1-` |
| `hmos-155-adapter` | `/mnt/chromium-155` | `depot155-linux-x64-v2-` | `depot155-apt-ubuntu24-x64-v1-` |

The cache paths, prefixes and configuration hash inputs are unchanged from
the successful trials. Moving CI to the release branches does not require a
new cache namespace. Restore prefixes never cross majors or configuration
hashes. The configuration hash covers the engine pin, GN arguments and SDK
manifest; each saved key also contains the adapter SHA and unique run ID.

Source, DEPS, SDK, compiler tools and `out/` live on the sandbox's local disk
and are restored/saved through `actions/cache`. The mounted cache filesystem
is not used for Git or native compilation. Completed APT `.deb` downloads
have their own cache; OS packages are installed in every new sandbox.

The selector verifies the actual checkout SHA against the explicitly named
published branch, accepting queued commits that remain ancestors of its
current head. It does not infer the engine from Depot CLI's synthesized
default-branch ref. The preparation script also verifies major, source path
and pinned revision before mutating the workspace.

154 applies its base adapter and incremental patches. 155 applies its
complete migration snapshots with digest receipts, preserving timestamps of
unchanged inputs. Both reuse completed compiler objects and run Ninja with
`-j16` on a 16-CPU / 64-GB Ubuntu 24.04 sandbox. Native compilation is serialized
within each major; the two majors have separate concurrency groups.

## Running and outputs

After pushing a clean checkout of the corresponding release branch, a manual
run can be started with:

```sh
depot ci run --org <org-id> --workflow _bak/depot-workflows/native-build.yml
```

The workflow validates native phishing rules, packages the runtime and
unstripped symbols, and uploads runtime/checksum/build metadata plus diagnostic
artifacts with seven-day retention. It saves the prepared workspace even if
compilation fails, but skips saving cancelled runs. The transferred CI is the
validated native workflow: HAR/HAP packaging and GitHub release publishing
remain separate from this job.

The first cache miss needs the private SDK subset matching
`config/depot-<major>-sdk.json`. Upload it over authenticated Depot sandbox
access, extract it at `/mnt/chromium-<major>/ohos_sdk`, and write its verified
archive SHA-256 to `/mnt/chromium-<major>/sdk-inbox/sdk.ready` after extraction.
Subsequent runs restore it from that major's cache. SDK contents and signing
material are not committed to Git.
