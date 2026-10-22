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

# Compile the production emergency check with a minimal CRLSet holder. The
# downloaded/previously-installed set may omit the incident; that must never
# remove the local floor. No network or Chromium build is involved.
floor = re.search(r'#if BUILDFLAG\(IS_OHOS\)\n  // Keep the emergency floor.*?#endif', ADDED, re.S)
assert floor
hash_literals = ','.join('std::string("' + ''.join('\\x' + value[i:i+2] for i in range(0, 64, 2))
                        + '",32)' for value in sorted(blocked))
unit = r'''
#include <algorithm>
#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#define BUILDFLAG(flag) 1
template<class T> using scoped_refptr = std::shared_ptr<T>;
namespace base {
template<class T> struct NoDestructor {
  explicit NoDestructor(T input) : value(std::move(input)) {}
  const T& operator*() const { return value; }
  T value;
};
}
struct CRLSet {
  enum Result { GOOD, REVOKED };
  std::vector<std::string> blocked_spkis_;
  static scoped_refptr<CRLSet> BuiltinCRLSet() {
    auto value=std::make_shared<CRLSet>();
    value->blocked_spkis_={HASH_LITERALS};
    std::sort(value->blocked_spkis_.begin(),value->blocked_spkis_.end());
    return value;
  }
  Result CheckSPKI(std::string_view spki_hash) const {
PRODUCTION_FLOOR
    if(std::binary_search(blocked_spkis_.begin(),blocked_spkis_.end(),spki_hash))return REVOKED;
    return GOOD;
  }
};
int main() {
  CRLSet old_download;
  const auto builtin=CRLSet::BuiltinCRLSet();
  for(const auto& hash:builtin->blocked_spkis_)
    assert(old_download.CheckSPKI(hash)==CRLSet::REVOKED);
  const std::string unrelated(32,'x');
  assert(old_download.CheckSPKI(unrelated)==CRLSet::GOOD);
  old_download.blocked_spkis_={unrelated};
  assert(old_download.CheckSPKI(unrelated)==CRLSet::REVOKED);
  for(const auto& hash:builtin->blocked_spkis_)
    assert(old_download.CheckSPKI(hash)==CRLSet::REVOKED);
}
'''.replace('HASH_LITERALS', hash_literals).replace('PRODUCTION_FLOOR', '\n'.join('  ' + line for line in floor[0].splitlines()))
with tempfile.TemporaryDirectory(prefix='ohos-crl-floor-') as directory:
    source = Path(directory) / 'floor.cc'
    binary = Path(directory) / 'floor'
    source.write_text(unit)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra',
                    '-Werror', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Integration contracts are checked here because this host test cannot create
# Chromium services. CRX signatures, persistence and live TLS need CI/devices.
assert 'if (!IsOhosSecurityComponent(component.app_id))' in ADDED
# Compile the actual added registration branch for both platform choices.
# The desktop sentinel must be excluded on OHOS, rather than left unreachable
# after an unconditional return (Chromium treats that warning as an error).
registration_patch = PATCH.split('diff --git a/chrome/browser/component_updater/registration.cc ', 1)[1].split('\ndiff --git ', 1)[0]
registration = '\n'.join(line[1:] for line in registration_patch.splitlines()
                         if line.startswith('+') and not line.startswith('+++'))
assert re.search(r'RegisterCRLSetComponent\(cus\);\s*MaybeRegisterPKIMetadataComponent\(cus\);\s*#else', registration)
registration = registration.replace('#else', '#else\n  RegisterDesktopComponent(cus);', 1)
with tempfile.TemporaryDirectory(prefix='ohos-registration-') as directory:
    source = Path(directory) / 'registration.cc'
    binary = Path(directory) / 'registration'
    for ohos in (0, 1):
        source.write_text('''
#include <cassert>
#define BUILDFLAG(flag) OHOS
int registered = 0;
void RegisterCRLSetComponent(int*) { registered |= 1; }
void MaybeRegisterPKIMetadataComponent(int*) { registered |= 2; }
void RegisterDesktopComponent(int*) { registered |= 4; }
void RegisterComponentsForUpdate() {
  int* cus = nullptr;
''' + registration + '''
}
int main() {
  RegisterComponentsForUpdate();
  assert(registered == (OHOS ? 3 : 4));
}
''')
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++20', '-Wall',
                        '-Wextra', '-Wunreachable-code-aggressive', '-Werror',
                        f'-DOHOS={ohos}', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
assert 'if (!mutable_command_line->HasSwitch("ohos-security-component-updates"))' in ADDED
assert 'if (url.SchemeIs("https"))' in ADDED
assert 'omit separate component telemetry pings' in ADDED
runner = (ROOT / 'overlay/arkweb/ohos_nweb/src/aura_shell/ohos_chrome_main_runner.cc').read_text()
assert '"--ohos-security-component-updates"' in runner
assert '"--disable-component-update"' not in runner
print('Security component allowlist, 16 certificate identities/eight SPKIs, and offline CRLSet format passed.')
