# OHOS certificate security component updates

The OHOS browser enables **CRLSet** (`hfnkpimlhhgieaddgfemjhofmfblmnib`)
and **PKI Metadata** (`efniojlnjndmcbiieegkicadnoecjjef`) only. Both are
platform-independent data signed with Chromium's pinned CRX component keys.
This does not enable Google account sync, arbitrary component registration,
speech/AI models, Widevine, or browser binary updates.

## Wiring

- The Aura browser adds `--ohos-security-component-updates`. The main delegate
  no longer forces `--disable-component-update` for that opt-in; an explicit
  caller-supplied disable switch is still honored. Other OHOS embedders retain
  their previous disabled default.
- `RegisterComponentsForUpdate()` registers just these two components on OHOS.
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

These hooks are carried by the already-applied `ohos-url-protection-updates.patch`
so the existing incremental and fresh-checkout paths see the same changes.
No CI workflow or build script changes are required.

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
2. Inspect `chrome://components`: exactly the two allowed security components
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
