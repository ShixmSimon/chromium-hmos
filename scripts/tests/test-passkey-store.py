#!/usr/bin/env python3
"""Fault-inject the real Add/Remove methods at their HUKS/file-I/O boundaries.

This small checkout has no Chromium base libraries. Compile the production
methods verbatim, supplying only record/IO doubles. JSON parsing, HUKS and
Chromium integration remain full-engine/device checks, not simulated here.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'overlay/device/fido/ohos/credential_store.cc').read_text()
methods = source[source.index('bool CredentialStore::Add('):source.index('bool CredentialStore::WriteAll(')]
prelude = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <vector>
using Id = std::vector<uint8_t>;
struct StoredCredential {std::string rp_id; struct {Id id;} user; Id credential_id;};
std::set<Id> keys;
std::vector<std::string> events;
bool delete_ok = true;
Id KeyAliasForCredentialId(const Id& id) {return id;}
bool DeleteSigningKey(const Id& id) {
  events.push_back("delete"); if (!delete_ok) return false;
  keys.erase(id); return true;
}
class CredentialStore {
 public:
  mutable std::vector<StoredCredential> disk;
  bool read_ok = true, write_ok = true;
  std::optional<std::vector<StoredCredential>> ReadAll() const {
    if (!read_ok)
      return std::nullopt;
    return disk;
  }
  bool WriteAll(const std::vector<StoredCredential>& records) const {
    events.push_back("write");
    if (!write_ok)
      return false;
    disk = records;
    return true;
  }
  bool Add(const StoredCredential&);
  bool Remove(const Id&);
};
'''
tests = r'''
int main() {
  const StoredCredential old{"site", {{1}}, {10}}, replacement{"site", {{1}}, {20}},
      other_user{"site", {{2}}, {11}}, other_site{"other", {{1}}, {12}};
  auto reset = [&] {keys = {{10}, {11}, {12}, {20}}; events.clear(); delete_ok=true;
    CredentialStore s; s.disk={old, other_user, other_site}; return s;};
  {
    auto s=reset(); s.write_ok=false;
    assert(!s.Add(replacement)); assert(keys.count({10}));
    assert(s.disk[0].credential_id == Id{10}); assert(events==std::vector<std::string>{"write"});
  }
  {
    auto s=reset(); assert(s.Add(replacement)); assert(!keys.count({10}));
    assert(keys.count({11}) && keys.count({12}) && keys.count({20}));
    assert(s.disk.size()==3 && s.disk.back().credential_id==Id{20});
    assert((events==std::vector<std::string>{"write", "delete"}));
  }
  {
    auto s=reset(); assert(s.Add(old)); assert(keys.count({10}));
    assert(events==std::vector<std::string>{"write"});
  }
  {
    auto s=reset(); s.read_ok=false;
    assert(!s.Add(replacement) && !s.Remove({10})); assert(keys.count({10})); assert(events.empty());
  }
  {
    auto s=reset(); s.write_ok=false;
    assert(!s.Remove({10})); assert(keys.count({10})); assert(s.disk.size()==3);
    assert(events==std::vector<std::string>{"write"});
  }
  {
    auto s=reset(); assert(s.Remove({10})); assert(!keys.count({10}));
    assert(s.disk.size()==2); assert((events==std::vector<std::string>{"write", "delete"}));
    events.clear(); assert(s.Remove({20})); assert(keys.count({20})); assert(events.empty());
  }
  {
    auto s=reset(); delete_ok=false; assert(s.Add(replacement));
    assert(s.disk.back().credential_id==Id{20} && keys.count({20}));
    // Failed best-effort cleanup cannot roll metadata back to a deleted key.
    assert(keys.count({10}));
  }
  puts("PASS passkey replacement/removal: write failures preserve keys; commit precedes cleanup; profile ownership preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='passkey-store-') as tmp:
    code = Path(tmp) / 'test.cc'
    code.write_text(prelude + methods + tests)
    binary = Path(tmp) / 'test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(code), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
