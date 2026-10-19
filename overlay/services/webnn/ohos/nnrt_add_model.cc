// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/webnn/ohos/nnrt_add_model.h"

#include <array>
#include <cstring>
#include <limits>
#include <utility>

#include <neural_network_runtime/neural_network_core.h>
#include <neural_network_runtime/neural_network_runtime.h>

namespace webnn::ohos {
namespace {

template <class T, auto Destroy>
struct NnrtDeleter {
  void operator()(T* value) const { Destroy(&value); }
};
using Model =
    std::unique_ptr<OH_NNModel, NnrtDeleter<OH_NNModel, OH_NNModel_Destroy>>;
using Compilation =
    std::unique_ptr<OH_NNCompilation,
                    NnrtDeleter<OH_NNCompilation, OH_NNCompilation_Destroy>>;
using Executor =
    std::unique_ptr<OH_NNExecutor,
                    NnrtDeleter<OH_NNExecutor, OH_NNExecutor_Destroy>>;
using Desc =
    std::unique_ptr<NN_TensorDesc,
                    NnrtDeleter<NN_TensorDesc, OH_NNTensorDesc_Destroy>>;
using Tensor =
    std::unique_ptr<NN_Tensor, NnrtDeleter<NN_Tensor, OH_NNTensor_Destroy>>;

bool Check(OH_NN_ReturnCode code, const char* operation, std::string& error) {
  if (code == OH_NN_SUCCESS) {
    return true;
  }
  error = std::string(operation) + " failed: " + std::to_string(code);
  return false;
}

Desc MakeDesc(const std::vector<int32_t>& shape,
              OH_NN_DataType type,
              std::string& error) {
  Desc desc(OH_NNTensorDesc_Create());
  if (!desc) {
    error = "OH_NNTensorDesc_Create failed";
    return nullptr;
  }
  if (!Check(OH_NNTensorDesc_SetShape(desc.get(), shape.data(), shape.size()),
             "SetShape", error) ||
      !Check(OH_NNTensorDesc_SetDataType(desc.get(), type), "SetDataType",
             error) ||
      !Check(OH_NNTensorDesc_SetFormat(desc.get(), OH_NN_FORMAT_NONE),
             "SetFormat", error)) {
    return nullptr;
  }
  return desc;
}

}  // namespace

struct NnrtAddModel::Impl {
  // Keep constant storage alive for the entire model lifetime.
  const int8_t activation = OH_NN_FUSED_NONE;
  // Reverse destruction keeps the model alive through compilation/execution.
  Model model;
  Compilation compilation;
  std::array<Tensor, 3> tensors;
  Executor executor;
  size_t bytes = 0;
};

bool FindNnrtAccelerator(NnrtDevice& device, std::string& error) {
  const size_t* ids = nullptr;
  uint32_t count = 0;
  if (!Check(OH_NNDevice_GetAllDevicesID(&ids, &count), "GetAllDevicesID",
             error)) {
    return false;
  }
  if (count && !ids) {
    error = "NNRt returned an invalid device list";
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    OH_NN_DeviceType type = OH_NN_OTHERS;
    if (OH_NNDevice_GetType(ids[i], &type) != OH_NN_SUCCESS ||
        type != OH_NN_ACCELERATOR) {
      continue;
    }
    const char* name = nullptr;
    if (!Check(OH_NNDevice_GetName(ids[i], &name), "GetName", error) || !name) {
      error = "NNRt accelerator has no device name";
      return false;
    }
    device = {ids[i], name};
    return true;
  }
  error = "No NNRt accelerator; CPU/GPU fallback is disabled";
  return false;
}

bool NnrtAddModel::ValidateShape(const std::vector<uint32_t>& shape,
                                 size_t& bytes,
                                 std::string& error) {
  bytes = 0;
  if (shape.empty() || shape.size() > 4) {
    error = "NNRt Add prototype supports ranks 1 through 4";
    return false;
  }
  size_t size = sizeof(float);
  for (uint32_t dim : shape) {
    if (!dim ||
        dim > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        dim > kMaxTensorBytes / size) {
      error = "Invalid shape or tensor exceeds 4 MiB";
      return false;
    }
    size *= dim;
  }
  bytes = size;
  return true;
}

std::unique_ptr<NnrtAddModel> NnrtAddModel::Create(
    const NnrtDevice& device,
    const std::vector<uint32_t>& shape,
    std::string& error) {
  auto impl = std::make_unique<Impl>();
  if (!ValidateShape(shape, impl->bytes, error)) {
    return nullptr;
  }
  // Recheck the selected device, including callers of the standalone probe.
  OH_NN_DeviceType type = OH_NN_OTHERS;
  if (!Check(OH_NNDevice_GetType(device.id, &type), "GetType", error) ||
      type != OH_NN_ACCELERATOR) {
    error = "Selected NNRt device is not an accelerator";
    return nullptr;
  }
  impl->model.reset(OH_NNModel_Construct());
  if (!impl->model) {
    error = "OH_NNModel_Construct failed";
    return nullptr;
  }
  const std::vector<int32_t> dims(shape.begin(), shape.end());
  auto desc = MakeDesc(dims, OH_NN_FLOAT32, error);
  if (!desc) {
    return nullptr;
  }
  for (uint32_t i = 0; i < 3; ++i) {
    if (!Check(OH_NNModel_AddTensorToModel(impl->model.get(), desc.get()),
               "AddTensorToModel", error) ||
        !Check(OH_NNModel_SetTensorType(impl->model.get(), i, OH_NN_TENSOR),
               "SetTensorType", error)) {
      return nullptr;
    }
  }
  auto activation_desc = MakeDesc({1}, OH_NN_INT8, error);
  if (!activation_desc ||
      !Check(
          OH_NNModel_AddTensorToModel(impl->model.get(), activation_desc.get()),
          "Add activation tensor", error) ||
      !Check(OH_NNModel_SetTensorType(impl->model.get(), 3,
                                      OH_NN_ADD_ACTIVATIONTYPE),
             "Set activation type", error) ||
      !Check(OH_NNModel_SetTensorData(impl->model.get(), 3, &impl->activation,
                                      sizeof(impl->activation)),
             "Set activation value", error)) {
    return nullptr;
  }
  uint32_t inputs[] = {0, 1};
  uint32_t output = 2;
  uint32_t activation = 3;
  const OH_NN_UInt32Array input_ids{inputs, 2};
  const OH_NN_UInt32Array output_ids{&output, 1};
  const OH_NN_UInt32Array params{&activation, 1};
  if (!Check(OH_NNModel_AddOperation(impl->model.get(), OH_NN_OPS_ADD, &params,
                                     &input_ids, &output_ids),
             "AddOperation", error) ||
      !Check(OH_NNModel_SpecifyInputsAndOutputs(impl->model.get(), &input_ids,
                                                &output_ids),
             "SpecifyInputsAndOutputs", error) ||
      !Check(OH_NNModel_Finish(impl->model.get()), "Finish", error)) {
    return nullptr;
  }
  const bool* supported = nullptr;
  uint32_t op_count = 0;
  if (!Check(OH_NNModel_GetAvailableOperations(impl->model.get(), device.id,
                                               &supported, &op_count),
             "GetAvailableOperations", error)) {
    return nullptr;
  }
  if (!supported || op_count != 1 || !supported[0]) {
    error = "Selected accelerator does not support this float32 Add graph";
    return nullptr;
  }
  impl->compilation.reset(OH_NNCompilation_Construct(impl->model.get()));
  if (!impl->compilation) {
    error = "OH_NNCompilation_Construct failed";
    return nullptr;
  }
  if (!Check(OH_NNCompilation_SetDevice(impl->compilation.get(), device.id),
             "SetDevice", error) ||
      !Check(OH_NNCompilation_EnableFloat16(impl->compilation.get(), false),
             "DisableFloat16", error) ||
      !Check(OH_NNCompilation_Build(impl->compilation.get()), "Build", error)) {
    return nullptr;
  }
  impl->executor.reset(OH_NNExecutor_Construct(impl->compilation.get()));
  if (!impl->executor) {
    error = "OH_NNExecutor_Construct failed";
    return nullptr;
  }
  for (auto& tensor : impl->tensors) {
    tensor.reset(OH_NNTensor_Create(device.id, desc.get()));
    if (!tensor) {
      error = "OH_NNTensor_Create failed";
      return nullptr;
    }
  }
  return std::unique_ptr<NnrtAddModel>(new NnrtAddModel(std::move(impl)));
}

NnrtAddModel::NnrtAddModel(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
NnrtAddModel::~NnrtAddModel() = default;
size_t NnrtAddModel::byte_length() const {
  return impl_->bytes;
}

bool NnrtAddModel::Run(const void* lhs,
                       size_t lhs_size,
                       const void* rhs,
                       size_t rhs_size,
                       void* output,
                       size_t output_size,
                       std::string& error) {
  if (!lhs || !rhs || !output || lhs_size != impl_->bytes ||
      rhs_size != impl_->bytes || output_size != impl_->bytes) {
    error = "NNRt Add buffer size mismatch";
    return false;
  }
  std::array<void*, 3> buffers{};
  for (size_t i = 0; i < buffers.size(); ++i) {
    buffers[i] = OH_NNTensor_GetDataBuffer(impl_->tensors[i].get());
    size_t capacity = 0;
    size_t offset = 0;
    if (!buffers[i] ||
        !Check(OH_NNTensor_GetSize(impl_->tensors[i].get(), &capacity),
               "GetSize", error) ||
        !Check(OH_NNTensor_GetOffset(impl_->tensors[i].get(), &offset),
               "GetOffset", error) ||
        offset > capacity || impl_->bytes > capacity - offset) {
      error = "Invalid NNRt tensor buffer or capacity";
      return false;
    }
  }
  std::memcpy(buffers[0], lhs, impl_->bytes);
  std::memcpy(buffers[1], rhs, impl_->bytes);
  NN_Tensor* inputs[] = {impl_->tensors[0].get(), impl_->tensors[1].get()};
  NN_Tensor* outputs[] = {impl_->tensors[2].get()};
  if (!Check(
          OH_NNExecutor_RunSync(impl_->executor.get(), inputs, 2, outputs, 1),
          "RunSync", error)) {
    return false;
  }
  std::memcpy(output, buffers[2], impl_->bytes);
  return true;
}

}  // namespace webnn::ohos
