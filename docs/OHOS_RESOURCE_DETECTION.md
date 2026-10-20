# Page resource detection, protocol v1

The first stage passively identifies successful HTTP(S) GET subresource
responses associated with a live document. MP4/WebM, audio, images, PDF and
attachment responses can be sent to Chromium's normal download manager. HLS
and DASH manifest candidates are displayed for copying only. This feature does
not parse manifests, combine tracks/segments, capture response bodies, decrypt
media, or turn an MSE blob URL into a complete file.

## Integration

- A browser-side URLLoaderFactoryBuilder interceptor is installed for document
  subresource factories, independently of adblock settings. It also sees loads
  issued by Blink background fetch threads. Worker-owned factories are excluded.
- The factory captures the source frame ID and committed/pending navigation ID.
  Clones retain that identity; responses must resolve to the same active,
  committed document. This avoids capturing the old WeakDocumentPtr before a
  reused frame commits. Accepted entries retain a WeakDocumentPtr for downloads.
  Ambiguous overlapping pending commits are excluded. A response
  observer inspects only headers, and forwards the original body pipe, metadata,
  loader control, redirects, progress and completion without buffering the body.
- Browser-owned per-WebContents state accepts active frames belonging to the
  current primary page, including cross-origin iframes. Primary page changes
  clear the collection and replace its opaque navigation identity. Request
  authorization and referrer remain native-only and are never sent to ArkUI.
- `resourcesChanged` carries only tab/navigation identity and is batched to
  at most once per 500 ms. The shell queries a full snapshot only while its
  resource view is open. Per-page caps: 128 resources, 32 images, 512 KiB of
  accounted metadata. No persistence, including in private windows.
- Resource keys preserve every query parameter, including signed URLs. HTTP
  byte-range retries of the same origin/URL update the existing entry. HTTP
  206 totals come from a validated Content-Range, never the chunk's length.

## Shell commands and events

Lumie exposes the list in its page menu. With Chromium native browser chrome,
the app supplies a localized label through `setResourceMenuLabel {name}`; the
native menu emits a window-targeted `resourcesRequested` event. The shell draws
only the resource overlay, without switching browser chrome or restarting.

- `listResources {requestId, tabId}` -> `resourceList {requestId, ok,
  protocolVersion:1, tabId, navigationId, resources, truncated}`.
- Each resource: opaque `id`, `name`, `url`, `kind` (`video`, `audio`, `image`,
  `file`, `hls`, `dash`), `mimeType`, `totalBytes` (-1 if unknown), `downloadable`.
- `downloadResource {requestId, tabId, navigationId, id}` ->
  `resourceActionResult`. Only a current, native-recorded resource is accepted;
  the shell cannot supply replacement URL/headers/path. A successful reply
  means a DownloadItem was created, not that its transfer completed.
- `clearResources {requestId, tabId, navigationId}` -> `resourceActionResult`
  with a new navigation identity; does not reload, delete downloads or cache.
- `resourcesChanged {tabId, navigationId}` invalidates an open view.

Commands are resolved against the requesting window's active tab. Stale tab,
document, resource and manifest downloads are rejected. Downloads are created
through the source RenderFrameHost, retaining Chromium security checks and its
storage partition. Original referrer/policy, credentials mode and
explicit Authorization are retained; Cookie is not extracted. Redirected
cross-origin requests drop captured Authorization. Range is not replayed, so
ordinary partial-file responses can request the full resource. Signed query
parameters are not rewritten. Normal download policies, warnings and failures
still apply; arbitrary request headers and POST replay are outside v1.

## Scope and validation

Known `.ts`, `.m4s` and segment MIME responses are excluded. A response whose
MIME/URL resembles a normal file can still be an application-specific stream
fragment; metadata alone cannot prove completeness or DRM status. Worker-only
requests, requests satisfied entirely by Blink memory cache, top-level file
navigation and response bodies are not covered by this factory interceptor. Ordinary
top-level downloads continue through Chromium as before. Disk-cache and
Service Worker behavior require device fixtures; no universal coverage claim.

Run `python3 scripts/tests/test-resource-policy.py` for the production classifier
and Content-Range parser (host-only C++).
`python3 scripts/tests/test-resource-document.py` compiles the production
identity resolver with a small host frame harness to exercise pending commits,
RFH reuse, inactivity and destruction. Lumie's
`node --test scripts/tests/resource-store.test.mjs` covers document/tab/request
identity, private/runtime isolation, old-engine timeouts, cancellation, manifest
rejection and coalesced refresh. Native compilation must run through the
existing push-triggered CI; do not modify CI workflows to add these tests.

Device validation should cover direct/signed/ranged media, iframe navigation,
redirects, wrong MIME/error responses, disabled adblock, private windows,
expired identities, manifest copy-only behavior and normal download creation.


## Verified build — 2026-10-20

Engine source `ef0c649f`, Lumie source `2f8abfad`:
[Native CI](https://github.com/miramira8295/chromium-hmos/actions/runs/39445403190)
and [Adapter CI](https://github.com/miramira8295/chromium-hmos/actions/runs/39445403262)
both completed successfully. Native binaries came from push-triggered CI; no CI
code was changed. Signed normal phone and Pad packages were assembled from that
HAR. The Pad package was installed; phone physical validation remains pending
because the phone was offline.

| Pad fixture check | Result |
| --- | --- |
| Video/audio, image, PDF, redirects, HLS/DASH classification | Passed: 9 expected entries |
| Signed URL, Cookie and Authorization download context | Passed: exact 2048-byte file |
| HTTP 206 size and subsequent full-file download without Range | Passed: 2048-byte total and exact file |
| HTML/error responses and known stream fragments | Excluded as expected |
| Cross-origin iframe, iframe navigation and primary navigation | Correct collection and expired-entry removal |
| Image, entry and accounted-metadata limits | Passed: 32 images, 128 entries, 512 KiB metadata limit |
| Same URL changes from video to HLS | Old download permission revoked |
| Chromium native menu opens shared resource UI | Passed on final build |
| Existing tabs and Vulkan backend | Original 9 tabs retained; Ganesh Vulkan; zero GPU crashes |

The exhaustive collector fixtures ran on `f6345843`; the production collector
and classification policy are byte-identical in final `ef0c649f`. The final
build additionally passed startup, nine-resource collection and native menu/UI
checks. Host production-policy and document-identity harnesses pass, as do all
179 Lumie Node tests. Test downloads and fixture cookies were removed.

These fixtures use synthetic local HTTP payloads to check response metadata,
body delivery and download integrity. They do not establish real-media decode,
DRM, stream merging, universal cache/worker coverage, or a complete Chromium
regression pass. Evidence and package provenance are retained under
`out/resource-detection-20261020/` locally (not committed).
