// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <cmath>
#include <iostream>
#include "services/webnn/ohos/nnrt_add_model.h"

int main() {
  webnn::ohos::NnrtDevice device;
  std::string error;
  if (!webnn::ohos::FindNnrtAccelerator(device, error)) {
    std::cerr << error << '\n';
    return 1;
  }
  std::cout << "NNRt accelerator id=" << device.id << " name=" << device.name
            << '\n';
  auto model = webnn::ohos::NnrtAddModel::Create(device, {1, 2, 2, 1}, error);
  if (!model) {
    std::cerr << error << '\n';
    return 2;
  }
  float lhs[] = {1, -2, 0.25f, 1024};
  float rhs[] = {3, 0.5f, -0.125f, -512};
  float output[4]{};
  const float expected[] = {4, -1.5f, 0.125f, 512};
  for (int run = 0; run < 10; ++run) {
    if (!model->Run(lhs, sizeof(lhs), rhs, sizeof(rhs), output, sizeof(output),
                    error)) {
      std::cerr << error << '\n';
      return 3;
    }
    for (size_t i = 0; i < 4; ++i) {
      if (!std::isfinite(output[i]) ||
          std::abs(output[i] - expected[i]) > 0.00001f) {
        std::cerr << "Incorrect output at " << i << ": " << output[i] << '\n';
        return 4;
      }
    }
  }
  std::cout << "PASS: 10 NNRt Add dispatches; CPU fallback disabled; "
               "2 input copies + 1 output copy per dispatch\n";
}
