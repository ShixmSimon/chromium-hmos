# OHOS security component updates

The OHOS browser enables an explicit allowlist of data components signed
with Chromium's pinned CRX component keys:

- **CRLSet** (`hfnkpimlhhgieaddgfemjhofmfblmnib`).
- **PKI Metadata** (`efniojlnjndmcbiieegkicadnoecjjef`).
- **Safety Tips** (`jflookgnkcckhobaglndicnbbgbonegd`).
- **Certificate Error Assistant** (`giekcmmlnklenlaomppkphknjmnnpneh`).
- **File Type Policies** (`khaoiebndkojlmppeemjhbpbandiljpe`).
- **Origin Trials** (`llkgjffcdpffmhiakmfcdcblohccpfmo`).
- **Hyphenation** (`jamhcnnkihinmdlkakkaopbjbbcngflc`).

This does not enable Google account sync, arbitrary component registration,
speech/AI models, Widevine, or browser binary updates.

## Wiring

- The Aura browser adds `--ohos-security-component-updates`. The main delegate
  no longer forces `--disable-component-update` for that opt-in; an explicit
  caller-supplied disable switch is still honored. Other OHOS embedders retain
  their previous disabled default.
- `RegisterComponentsForUpdate()` registers only the explicitly allowed components on OHOS.
  The component service also rejects every other ID, including registrations
  attempted later by individual features. SODA installation stays disabled.
- Existing `disable-background-networking` and sync restrictions remain.
  The component service's own network fetcher has no dependency on the former
  switch, and uses a Cookie-omitting system network context.
- Update checks and component packages require HTTPS. Separate component ping
  requests are disabled. Checks still disclose the connection IP and upstream
  protocol metadata (browser/platform/component versions, architecture and
  protocol-provided machine information); this is not anonymous networking.
- CRX signature, pinned publisher-key, package-hash and component installer
  verification remain upstream implementations. A fetched raw JSON or CRL file
  is not accepted as a network update.
- Existing component registration loads verified installed data on startup.
  `CRLSetPolicy::ComponentReady()` updates `CertVerifierServiceFactory`;
  `OnCRLSetParsed()` rejects equal/older sequences and updates existing verifier
  services. PKI Metadata supplies the Chrome Root Store, CT and supported PKI
  data through its existing compatibility checks. Its upstream root-store
  rollback rules intentionally differ from the CRLSet monotonic sequence rule.
- INFO diagnostics report an actually accepted CRLSet sequence or an applied
  root-store update, without certificate subjects, browsing URLs, or user data.

The original certificate hooks are carried by
`ohos-url-protection-updates.patch` (included in the 155 migration snapshot).
The existing incremental build applies `ohos-security-assistants.patch` after
those prerequisites on both 154 and 155. No additional workflow is required.

## Safety Tips and Certificate Error Assistant

Safety Tips uses the upstream installer and pinned publisher key to download
`safety_tips.pb`. `SetSafetyTipsRemoteConfigProto()` feeds the existing
`LookalikeUrlService`, which previously skipped checks when this config was
absent. The primary-navigation throttle can display a lookalike interstitial;
`SafetyTipWebContentsObserver`, already attached to tabs, can display a warning
bubble for the applicable heuristic. The existing OHOS page-info anchor places
that bubble below the shell controls. This is a separate Chromium protection,
not a downloaded adblock/phishing subscription or a replacement for one.

The assistant installer loads `ssl_error_assistant.pb` through
`SSLErrorHandler::SetErrorAssistantProto()`. The existing SSL navigation error
handler uses it for captive-portal, interception-software and dynamic error-page
diagnostics. It does not grant trust to invalid certificates. Upstream builtin
configuration comparison and rejection of older/equal active versions remain.

Both installers require HTTPS on OHOS. Successful activation logs contain only
configuration versions/counts, no visited domains or certificate subjects:

- `OHOS security data: active Safety Tips version=...`
- `OHOS security data: active Certificate Error Assistant version=...`

An unreadable/malformed Safety Tips file leaves the previous active config in
place on OHOS. With no valid initial config, lookalike checks retain upstream's
no-config behavior; a component row alone does not establish protection.

On 2026-11-04, an official update check for OHOS/arm64 advertised Safety Tips
3092 and Certificate Error Assistant 7. Both HTTPS packages were downloaded,
matched to the response's SHA-256, and their CRX3 signatures were verified with
the public-key hashes pinned in the corresponding Chromium 155 installers.
The payloads contained proto versions 3092 and 7; Safety Tips had 5,371 allowed
patterns and four heuristic launch configs. Public package metadata is recorded
in `scripts/tests/fixtures/security-components/assistants-20261104.json`.
This is host verification, not on-device download/activation or UI acceptance.

Device acceptance for the assistants: trigger updates in `chrome://components`,
confirm both activation logs, restart and confirm persisted reload. Verify a
controlled lookalike URL through a local host-resolver mapping (never navigate
to an arbitrary real typo domain), normal-site/allowlist behavior, warning
buttons and shell placement on phone/Pad. Exercise certificate error handling
with a local TLS fixture; component diagnostics must not bypass certificate
validation. These device checks remain pending for the new build.

## Offline emergency floor

An ordinary Chromium 154 builtin CRLSet is empty, sequence zero. The OHOS
builtin now blocks the eight compromised eMudhra intermediate **SPKI SHA-256**
hashes, covering the sixteen original/cross-signed certificate identities.
This is an emergency denylist, not a complete downloaded CRLSet. It deliberately
retains sequence zero so any newer verified official CRLSet can replace its
ordinary revocation data. `CheckSPKI()` always also checks this emergency floor,
so an already-installed signed component from before the incident cannot remove
these blocks. Removing a compromised-key block requires an explicit code change.
It is immediately available before component downloads and on an offline first
launch. It does not distrust unrelated certificates by CA name, parent root,
or website domain.

Sources:

- [Mozilla CA incident 2083449](https://bugzilla.mozilla.org/show_bug.cgi?id=2083449)
- [Public intermediate certificate inventory](https://emudhra.we-have-a-problem.com/impacted-intermediates.json)
- [Public certificates and proof](https://emudhra.we-have-a-problem.com/proof.zip)
- [Chromium CRLSet mechanism](https://www.chromium.org/Home/chromium-security/crlsets/)

The public proof archive's eight certificates were decoded using OpenSSL;
each DER certificate SHA-256 and DER SPKI SHA-256 was independently matched to
the inventory. The committed fixture contains only public CA names and hashes.
No private keys, device certificates, signing profiles or device logs belong
in this fixture.

## Validation and remaining device checks

`python3 scripts/tests/test-security-components.py` compiles the production
component-ID policy in a small host harness and checks the actual builtin
CRLSet byte format against all sixteen public certificate identities. It is
not a Chromium build or a live TLS verification test.

On 2026-10-22, an HTTPS update protocol request using `OHOS`/`ohos` and `arm64`
received CRLSet **10852** and PKI Metadata **1810** from the official server;
both advertised HTTPS package mirrors. No OS identity substitution was needed.
Both downloaded CRX3 packages passed independent SHA-256 and pinned publisher
signature verification. The actual CRLSet 10852 contains all six affected TLS
SPKIs (the remaining two compromised keys are code-signing CAs).
The modified platform files also passed forward and reverse patch application
against the pinned 154.0.8037.97 upstream files plus the platform prerequisites.

Required CI/device acceptance:

1. Compile through push-triggered CI; package phone and Pad from that HAR.
2. Inspect `chrome://components`: the allowed security components
   register. Check update failures separately from up-to-date status.
3. Confirm accepted-sequence/root-store diagnostics, HTTPS transfers and no
   Cookie or separate ping requests. Listing an installed component alone is
   not proof its certificate data was loaded.
4. Check persisted reload after restart; old/malformed CRLSets must not replace
   newer active data. Repeat with the network service restarted.
5. Verify eMudhra chains (including alternate cross-signs) fail certificate
   validation on phone and Pad, online and offline; normal HTTPS must continue
   working. Record the certificate error, not just a fetch/CORS/network failure.
6. With no existing profile data and networking disabled, verify the builtin
   emergency block already applies. A test run using an existing downloaded
   CRLSet does not establish this first-run guarantee.


## Additional data components: File Type Policies

The additive `ohos-file-type-component.patch` enables the local file-type
policy library, built-in data resource and signed updater even when
`safe_browsing_mode=0`. It does not enable online Safe Browsing checks.
`DownloadTargetDeterminer::GetDangerLevel` and DownloadPrefs auto-open checks
consume the active policy on OHOS; existing dangerous-download events and
keep/discard UI remain responsible for user decisions. The installer requires
HTTPS; package/hash/pinned CRX verification and protobuf version checks stay
upstream. An INFO log reports dynamic-load result and active policy version,
not filenames or visited URLs. Unknown component IDs still fail closed.

The patch applies against both pinned 154 and migrated 155 sources. Host
allowlist/security regression tests passed. Full native build and device
acceptance are pending; registration alone is not proof of active policy.

## Origin Trials

`ohos-origin-trials-component.patch` registers the pinned
`llkgjffcdpffmhiakmfcdcblohccpfmo` component and requires HTTPS on OHOS.
The existing Chrome installer writes the manifest configuration to local
state. `ChromeBrowserMainParts` already invokes
`SetupOriginTrialsCommandLineAndSettings` and distributes those settings to
Blink at startup, so changes take effect after restarting Lumie. This does not
force-enable all experimental APIs. The diagnostic reports only staged
component version/restart requirement. Both pinned branches pass patch
forward/reverse checks and the allowlist host checks; native/device acceptance
is still pending.

## Hyphenation

`ohos-hyphenation-component.patch` registers dictionaries behind the existing
`USE_MINIKIN_HYPHENATION` build flag (already enabled on OHOS). The installer
publishes its directory to `ChromeContentBrowserClient`'s non-Android dictionary
service; Blink's Minikin implementation requests and maps language dictionaries
through that service. No separate renderer downloader is introduced. HTTPS and
the upstream pinned CRX key are required. The ready log reports the version.
Both pinned branches pass patch forward/reverse checks and host policy checks.
Native/device acceptance remains pending: update the component, then compare a
narrow English paragraph with `lang=en; hyphens:auto` against `hyphens:none`.
