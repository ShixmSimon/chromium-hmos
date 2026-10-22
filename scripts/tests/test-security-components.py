#!/usr/bin/env python3
"""Host checks for the production OHOS component policy and offline CRL floor.

This compiles only a tiny policy harness, not Chromium. Native builds use CI.
"""
import base64
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PATCH = (ROOT / 'patches/ohos-url-protection-updates.patch').read_text()
ADDED = '\n'.join(line[1:] for line in PATCH.splitlines()
                  if line.startswith('+') and not line.startswith('+++'))
FIXTURE = json.loads((ROOT / 'scripts/tests/fixtures/security-components/emudhra-intermediates.json').read_text())

with tempfile.TemporaryDirectory(prefix='ohos-security-policy-') as directory:
    source = Path(directory) / 'policy.cc'
    binary = Path(directory) / 'policy'
    source.write_text(r'''
#include <cassert>
#include "components/component_updater/ohos_security_component_policy.h"
using component_updater::IsOhosSecurityComponent;
int main() {
  static_assert(IsOhosSecurityComponent("hfnkpimlhhgieaddgfemjhofmfblmnib"));
  static_assert(IsOhosSecurityComponent("efniojlnjndmcbiieegkicadnoecjjef"));
  for (auto id : {"", "CRLSet", "PKI Metadata", "widevine",
                  "hfnkpimlhhgieaddgfemjhofmfblmnia",
                  "hfnkpimlhhgieaddgfemjhofmfblmnib/../other",
                  "Hfnkpimlhhgieaddgfemjhofmfblmnib"})
    assert(!IsOhosSecurityComponent(id));
}
''')
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra',
                    '-Werror', '-I' + str(ROOT / 'overlay'), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Decode the actual literal installed in CRLSet::BuiltinCRLSet, including its
# little-endian length. Protect original and cross-signed certificate identities.
literal = re.search(r'"((?:\\x[0-9a-f]{2}){2})"\s*R"crl\((.*?)\)crl"', ADDED, re.S)
assert literal, 'Production offline CRLSet literal missing'
prefix = bytes(int(value, 16) for value in re.findall(r'\\x([0-9a-f]{2})', literal[1]))
header = literal[2].encode()
assert struct.unpack('<H', prefix)[0] == len(header)
data = json.loads(header)
assert data['ContentType'] == 'CRLSet' and data['Version'] == 0
assert data['Sequence'] == 0  # Never claim the sequence of a full downloaded set.
blocked = {base64.b64decode(value, validate=True).hex().upper()
           for value in data['BlockedSPKIs']}
assert len(blocked) == 8 and all(len(value) == 64 for value in blocked)
assert len(FIXTURE['certificates']) == 16
assert blocked == {cert['spki_sha256'] for cert in FIXTURE['certificates']}
assert '00' * 32 not in blocked

# Integration contracts are checked here because this host test cannot create
# Chromium services. CRX signatures, persistence and live TLS need CI/devices.
assert 'if (!IsOhosSecurityComponent(component.app_id))' in ADDED
assert re.search(r'RegisterCRLSetComponent\(cus\);\s*MaybeRegisterPKIMetadataComponent\(cus\);\s*return;', ADDED)
assert 'if (!mutable_command_line->HasSwitch("ohos-security-component-updates"))' in ADDED
assert 'if (url.SchemeIs("https"))' in ADDED
assert 'omit separate component telemetry pings' in ADDED
runner = (ROOT / 'overlay/arkweb/ohos_nweb/src/aura_shell/ohos_chrome_main_runner.cc').read_text()
assert '"--ohos-security-component-updates"' in runner
assert '"--disable-component-update"' not in runner
print('Security component allowlist, 16 certificate identities/eight SPKIs, and offline CRLSet format passed.')
