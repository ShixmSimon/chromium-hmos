#!/usr/bin/env python3
"""Host-only regression of the production document identity resolver."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'overlay/chrome/browser/ohos/resource_collector.cc').read_text()
start = source.index('struct SourceDocument {')
end = source.index('\n};',start)+3
production = source[start:end]
unit = r'''
#include <cassert>
#include <cstdint>
namespace content {
struct GlobalRenderFrameHostId { int id; };
struct RenderFrameHost {
  static inline RenderFrameHost* current = nullptr;
  int id = 7;
  int64_t navigation = 11;
  bool active = true;
  static RenderFrameHost* FromID(GlobalRenderFrameHostId id) {
    return current && current->id == id.id ? current : nullptr;
  }
  bool IsActive() const { return active; }
  int64_t GetNavigationId() const { return navigation; }
};
}
PRODUCTION
int main() {
  content::RenderFrameHost frame;
  content::RenderFrameHost::current=&frame;
  SourceDocument original{{7},11}, pending{{7},12}, other{{8},11};
  assert(original.Resolve()==&frame);
  assert(pending.Resolve()==nullptr);  // A pending new document is not current.
  assert(other.Resolve()==nullptr);
  frame.active=false;
  assert(original.Resolve()==nullptr);
  frame.active=true;
  frame.navigation=12;  // Reused RFH commits; old document IDs cannot cross it.
  assert(original.Resolve()==nullptr);
  assert(pending.Resolve()==&frame);
  frame.navigation=13;
  assert(original.Resolve()==nullptr && pending.Resolve()==nullptr);
  content::RenderFrameHost::current=nullptr;
  assert(pending.Resolve()==nullptr);
}
'''.replace('PRODUCTION',production)
with tempfile.TemporaryDirectory(prefix='resource-document-') as directory:
    path=Path(directory)/'document.cc';binary=Path(directory)/'document'
    path.write_text(unit)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror',
                    str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('Production resource identity rejects pending, replaced, inactive and destroyed documents.')
