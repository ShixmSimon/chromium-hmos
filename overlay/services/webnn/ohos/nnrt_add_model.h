// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_WEBNN_OHOS_NNRT_ADD_MODEL_H_
#define SERVICES_WEBNN_OHOS_NNRT_ADD_MODEL_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace webnn::ohos {

// Small, deliberately restricted bring-up backend. All calls, including
// destruction, must run on the same NNRt thread. No CPU fallback.
struct NnrtDevice {
  size_t id;
  std::string name;
};

bool FindNnrtAccelerator(NnrtDevice& device, std::string& error);

class NnrtAddModel {
 public:
  static constexpr size_t kMaxTensorBytes = 4 * 1024 * 1024;
  static bool ValidateShape(const std::vector<uint32_t>& shape,
                            size_t& bytes,
                            std::string& error);
  static std::unique_ptr<NnrtAddModel> Create(
      const NnrtDevice& device,
      const std::vector<uint32_t>& shape,
      std::string& error);
  ~NnrtAddModel();
  NnrtAddModel(const NnrtAddModel&) = delete;
  NnrtAddModel& operator=(const NnrtAddModel&) = delete;

  size_t byte_length() const;
  // Copies into/out of device tensors. Success is reported only after RunSync
  // succeeds. The output is untouched on failure. Buffers may alias.
  bool Run(const void* lhs,
           size_t lhs_size,
           const void* rhs,
           size_t rhs_size,
           void* output,
           size_t output_size,
           std::string& error);

 private:
  struct Impl;
  explicit NnrtAddModel(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace webnn::ohos
#endif  // SERVICES_WEBNN_OHOS_NNRT_ADD_MODEL_H_
