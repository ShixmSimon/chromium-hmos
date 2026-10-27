#include "base/process/ohos_jit_startup.h"

#include <cassert>
#include <iostream>
#include <vector>

using namespace base::internal;

struct Scenario {
  std::vector<JitMemoryProbe> results;
  JitCompatibilityRequest reply{0, 0};
  size_t probes = 0;
  int requests = 0;

  JitStartupResult Run(bool disabled = false, bool allow = true) {
    return PrepareJitStartup(disabled, allow,
                             [&] { return results.at(probes++); },
                             [&] {
                               assert(probes == 1);
                               ++requests;
                               return reply;
                             });
  }
};

int main() {
  const JitMemoryProbe available{true, 0, 0, 0};
  const JitMemoryProbe denied{false, 0, EINVAL, 0};
  Scenario disabled;
  assert(disabled.Run(true).disabled);
  assert(disabled.probes == 0 && disabled.requests == 0);

  Scenario ready{{available}};
  assert(ready.Run().after.available);
  assert(ready.probes == 1 && ready.requests == 0);

  Scenario opt_out{{denied}};
  assert(!opt_out.Run(false, false).after.available);
  assert(opt_out.probes == 1 && opt_out.requests == 0);

  for (int error : {EINVAL, EACCES, EPERM}) {
    Scenario enabled{{{false, 0, error, 0}, available}};
    auto result = enabled.Run();
    assert(result.after.available && result.request_attempted);
    assert(result.before.protect_error == error);
    assert(enabled.probes == 2 && enabled.requests == 1);
  }

  Scenario false_success{{denied, denied}};
  auto result = false_success.Run();
  assert(result.request.result == 0 && !result.after.available);
  assert(false_success.probes == 2 && false_success.requests == 1);

  Scenario rejected{{denied, denied}, {-1, EPERM}};
  result = rejected.Run();
  assert(!result.after.available && result.request.error == EPERM);
  assert(rejected.probes == 2 && rejected.requests == 1);

  Scenario changed_elsewhere{{denied, available}, {-1, EINVAL}};
  assert(changed_elsewhere.Run().after.available);

  for (JitMemoryProbe failure : {JitMemoryProbe{false, ENOMEM, 0, 0},
                                 JitMemoryProbe{false, 0, ENOMEM, 0},
                                 JitMemoryProbe{false, 0, EINVAL, EIO}}) {
    Scenario failed{{failure}};
    assert(!failed.Run().after.available);
    assert(failed.probes == 1 && failed.requests == 0);
  }

  assert(JitlessRequested("--trace-gc --jitless"));
  assert(JitlessRequested("--jitless=true,--expose-gc"));
  assert(JitlessRequested("--jitless=1\t--wasm-jitless"));
  assert(JitlessRequested("--no-jitless --jitless"));
  assert(!JitlessRequested("--wasm-jitless --trace-gc"));
  assert(!JitlessRequested("--jitless=false"));
  assert(!JitlessRequested("--jitless --nojitless"));
  assert(!JitlessRequested("--jitless --no-jitless"));
  assert(!JitlessRequested("--jitless --jitless=0"));
  assert(!JitlessRequested("--jitless-other"));
  assert(!JitlessRequested(" \t\n"));
  std::cout << "PASS: JIT opt-out, existing capability, request/probe failures, "
               "bounded retry and explicit V8 flags\n";
}
