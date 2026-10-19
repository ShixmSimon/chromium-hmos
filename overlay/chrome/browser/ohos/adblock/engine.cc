// Copyright 2026 The Chromium Authors. BSD-style license in LICENSE.
#include "chrome/browser/ohos/adblock/engine.h"
#include <dlfcn.h>
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/sequenced_task_runner.h"
namespace chrome::ohos::adblock {
namespace {
struct Api {
  // Deliberately separate DSO: Chromium and Cargo must not share Rust runtime
  // or allocator symbols.
  void* library = dlopen("liblumie_adblock.so", RTLD_NOW | RTLD_LOCAL);
  using Version = uint32_t (*)();
  Version version = library ? reinterpret_cast<Version>(
                                  dlsym(library, "lumie_adblock_abi_version"))
                            : nullptr;
  using Compile = void* (*)(const char*, size_t, char**);
  using Query = char* (*)(const void*, const char*, size_t);
  using Free = void (*)(void*);
  using FreeString = void (*)(char*);
  Compile compile =
      library
          ? reinterpret_cast<Compile>(dlsym(library, "lumie_adblock_compile"))
          : nullptr;
  Query query =
      library ? reinterpret_cast<Query>(dlsym(library, "lumie_adblock_query"))
              : nullptr;
  Free free = library
                  ? reinterpret_cast<Free>(dlsym(library, "lumie_adblock_free"))
                  : nullptr;
  FreeString free_string =
      library ? reinterpret_cast<FreeString>(
                    dlsym(library, "lumie_adblock_free_string"))
              : nullptr;
  bool valid() const {
    return version && version() == 2 && compile && query && free && free_string;
  }
};
Api& GetApi() {
  static Api api;
  return api;
}
base::DictValue Decode(char* result) {
  if (!result)
    return {};
  auto value = base::JSONReader::ReadDict(result, base::JSON_PARSE_RFC);
  GetApi().free_string(result);
  return value ? std::move(*value) : base::DictValue();
}
}  // namespace
std::shared_ptr<Engine> Engine::Compile(const base::DictValue& input,
                                        base::DictValue* info) {
  auto& api = GetApi();
  if (!api.valid()) {
    info->Set("reason", "engineUnavailable");
    return nullptr;
  }
  std::string json;
  if (!base::JSONWriter::Write(input, &json))
    return nullptr;
  char* raw_info = nullptr;
  void* handle = api.compile(json.data(), json.size(), &raw_info);
  *info = Decode(raw_info);
  if (!handle)
    return nullptr;
  auto runner = base::SequencedTaskRunner::GetCurrentDefault();
  return std::shared_ptr<Engine>(new Engine(handle), [runner](Engine* engine) {
    runner->DeleteSoon(FROM_HERE, engine);
  });
}
Engine::~Engine() {
  GetApi().free(handle_);
}
base::DictValue Engine::Query(const base::DictValue& input) const {
  std::string json;
  if (!base::JSONWriter::Write(input, &json))
    return {};
  return Decode(GetApi().query(handle_, json.data(), json.size()));
}
}  // namespace chrome::ohos::adblock
