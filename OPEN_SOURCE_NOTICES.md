# Open Source Notices

This source publication corresponds to an unofficial Chromium for HarmonyOS
test build based on Chromium `150.0.7871.114` at revision
`f405107495a07cb1bfcf687d4af8d91117098db6`.

The target-level Chromium license scan identified, among many permissively
licensed dependencies, the following reciprocal components:

- FFmpeg: BSD-2-Clause, BSD-3-Clause, ISC, LGPL-2.1+, and MIT components.
- WebKit/Blink covered files: BSD, LGPL-2, and LGPL-2.1.
- Hunspell: MPL-1.1.
- Symphonia crates used by Chromium: MPL-2.0.

The tested FFmpeg configuration used `ffmpeg_branding="Chromium"`,
`proprietary_codecs=false`, `CONFIG_GPL=0`, `CONFIG_NONFREE=0`, and
`CONFIG_VERSION3=0`. No FFmpeg GPL or nonfree configuration was enabled.

Chromium's BSD license is available as `LICENSE`. Relevant LGPL, MPL, Apache,
and Blink/WebKit texts are under `licenses/`. The generated Chromium credits
page is available as `licenses/THIRD_PARTY_LICENSES.html`.

FFmpeg is statically included in the monolithic Chromium native library for
this configuration. The pinned upstream source, full patch, new source overlay,
GN arguments, and build notes are published so recipients can inspect, modify,
rebuild, and relink the applicable components. No term in this repository is
intended to prohibit reverse engineering for debugging such modifications.

## Optional user-selected threat lists

The rule-source catalog references HaGeZi DNS Blocklists (GPL-3.0) and
PhishDestroy Destroylist (MIT, Copyright (c) 2019 PhishDestroy). Their complete
lists are not bundled in the source publication or the engine HAR. Users may
explicitly download the unmodified lists from the publishers; the source API
exposes attribution and upstream license links. CI benchmark downloads retain
the license beside each pinned data file and do not become runtime assets.
See `docs/OHOS_PROTECTION_RULE_SOURCES.md` for URLs and redistribution notes.

