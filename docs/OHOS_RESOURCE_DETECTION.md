# Page resource detection, protocol v1

The first stage passively identifies successful HTTP(S) GET subresource
responses associated with a live document. MP4/WebM, audio, images, PDF and
attachment responses can be sent to Chromium's normal download manager. HLS
and DASH manifest candidates are displayed for copying only. This feature does
not parse manifests, combine tracks/segments, capture response bodies, decrypt
media, or turn an MSE blob URL into a complete file.

## Integration

- A standalone renderer `URLLoaderThrottle` is installed alongside adblock's
  throttle, independently of adblock settings. It filters by response MIME,
  path and disposition; only candidate metadata crosses Mojo. Scripts, CSS and
  font destinations do not allocate observation pipes.
- The Mojo receiver is a frame-bound `DocumentService`, destroyed across
  cross-document navigation. It never trusts a renderer-supplied tab ID.
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
storage partition. Original referrer/policy, credentials mode, request mode and
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
navigation and response bodies are not covered by this throttle. Ordinary
top-level downloads continue through Chromium as before. Disk-cache and
Service Worker behavior require device fixtures; no universal coverage claim.

Run `python3 scripts/tests/test-resource-policy.py` for the production classifier
and Content-Range parser (host-only C++). Lumie's
`node --test scripts/tests/resource-store.test.mjs` covers document/tab/request
identity, private/runtime isolation, old-engine timeouts, cancellation, manifest
rejection and coalesced refresh. Native compilation must run through the
existing push-triggered CI; do not modify CI workflows to add these tests.

Device validation should cover direct/signed/ranged media, iframe navigation,
redirects, wrong MIME/error responses, disabled adblock, private windows,
expired identities, manifest copy-only behavior and normal download creation.
