// Copyright (c) 2026
// Licensed under the Apache License, Version 2.0.

#ifndef BASE_PROCESS_OHOS_JIT_MEMORY_H_
#define BASE_PROCESS_OHOS_JIT_MEMORY_H_

#include <sys/mman.h>
#include <unistd.h>

#include "base/process/ohos_jit_startup.h"

namespace base::internal {

// ARM64 V8's default external CodeRange is 256 MiB (v8/src/common/globals.h).
// Reserve address space, never populate the whole range. A one-page RW -> RX
// probe does not cover CodeRange::InitReservation's RWX + discard operations.
inline constexpr size_t kOhosJitProbeRangeSize = 256 * 1024 * 1024;

struct OhosJitMemoryOperations {
  long PageSize() const { return sysconf(_SC_PAGESIZE); }
  void* Map(size_t size) const {
    return mmap(nullptr, size, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  }
  int Protect(void* address, size_t size, int protection) const {
    return mprotect(address, size, protection);
  }
  int Discard(void* address, size_t size) const {
    return madvise(address, size, MADV_DONTNEED);
  }
  int Unmap(void* address, size_t size) const { return munmap(address, size); }
};

template <typename Memory>
JitMemoryProbe ProbeOhosJitMemoryWith(Memory memory) {
  JitMemoryProbe result;
  const long page_size = memory.PageSize();
  if (page_size <= 0 ||
      kOhosJitProbeRangeSize % static_cast<size_t>(page_size)) {
    result.map_error = EINVAL;
    result.failure_stage = "page-size";
    return result;
  }
  void* range = memory.Map(kOhosJitProbeRangeSize);
  if (range == MAP_FAILED) {
    result.map_error = errno;
    result.failure_stage = "reserve";
    return result;
  }
  if (memory.Protect(range, kOhosJitProbeRangeSize,
                     PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    result.protect_error = errno;
    result.failure_stage = "code-range-rwx";
  } else if (memory.Discard(range, kOhosJitProbeRangeSize) != 0) {
    result.discard_error = errno;
    result.failure_stage = "code-range-discard";
  } else if (memory.Protect(range, page_size, PROT_READ | PROT_WRITE) != 0) {
    result.protect_error = errno;
    result.failure_stage = "page-rw";
  } else {
    *static_cast<volatile unsigned char*>(range) = 0;
    if (memory.Protect(range, page_size, PROT_READ | PROT_EXEC) != 0) {
      result.protect_error = errno;
      result.failure_stage = "page-rx";
    }
  }
  result.unmap_error =
      memory.Unmap(range, kOhosJitProbeRangeSize) == 0 ? 0 : errno;
  if (!result.failure_stage && result.unmap_error) {
    result.failure_stage = "cleanup";
  }
  result.available = !result.map_error && !result.protect_error &&
                     !result.discard_error && !result.unmap_error;
  return result;
}

inline JitMemoryProbe ProbeOhosJitMemory() {
  return ProbeOhosJitMemoryWith(OhosJitMemoryOperations{});
}

}  // namespace base::internal

#endif  // BASE_PROCESS_OHOS_JIT_MEMORY_H_
