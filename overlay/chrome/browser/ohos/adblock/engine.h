// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#ifndef CHROME_BROWSER_OHOS_ADBLOCK_ENGINE_H_
#define CHROME_BROWSER_OHOS_ADBLOCK_ENGINE_H_
#include <memory>
#include "base/values.h"
namespace chrome::ohos::adblock {
// Only used on the service's sequenced worker. No Rust allocator crosses the
// ABI.
class Engine {
 public:
  static std::shared_ptr<Engine> Compile(const base::DictValue& input,
                                         base::DictValue* info);
  ~Engine();
  base::DictValue Query(const base::DictValue& input) const;

 private:
  explicit Engine(void* handle) : handle_(handle) {}
  void* handle_;
};
}  // namespace chrome::ohos::adblock
#endif
