// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// A fault-injecting SDK boundary: this tests host logic/ownership, NOT NPU
// support.
#include <neural_network_runtime/neural_network_core.h>
#include <neural_network_runtime/neural_network_runtime.h>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include "services/webnn/ohos/nnrt_add_model.h"

struct NN_TensorDesc {
  size_t bytes = 0;
};
struct OH_NNModel {};
struct OH_NNCompilation {};
struct OH_NNExecutor {};
struct NN_Tensor {
  std::vector<float> data;
};
namespace {
int live = 0, calls = 0, fail_at = -1;
bool accelerator = true, supported = true, small_buffer = false;
size_t chosen = 0;
bool fail() {
  return ++calls == fail_at;
}
template <class T>
T* create() {
  if (fail())
    return nullptr;
  ++live;
  return new T;
}
template <class T>
void destroy(T** p) {
  assert(p && *p);
  delete *p;
  *p = nullptr;
  --live;
}
OH_NN_ReturnCode status() {
  return fail() ? OH_NN_FAILED : OH_NN_SUCCESS;
}
}  // namespace
extern "C" {
OH_NN_ReturnCode OH_NNDevice_GetAllDevicesID(const size_t** ids, uint32_t* n) {
  static const size_t values[] = {10, 20};
  *ids = values;
  *n = 2;
  return status();
}
OH_NN_ReturnCode OH_NNDevice_GetType(size_t id, OH_NN_DeviceType* type) {
  *type = (id == 20 && accelerator) ? OH_NN_ACCELERATOR : OH_NN_CPU;
  return status();
}
OH_NN_ReturnCode OH_NNDevice_GetName(size_t id, const char** name) {
  assert(id == 20);
  *name = "fake-accelerator";
  return status();
}
NN_TensorDesc* OH_NNTensorDesc_Create() {
  return create<NN_TensorDesc>();
}
OH_NN_ReturnCode OH_NNTensorDesc_Destroy(NN_TensorDesc** p) {
  destroy(p);
  return OH_NN_SUCCESS;
}
OH_NN_ReturnCode OH_NNTensorDesc_SetShape(NN_TensorDesc* d,
                                          const int32_t* shape,
                                          size_t n) {
  d->bytes = 4;
  for (size_t i = 0; i < n; ++i)
    d->bytes *= shape[i];
  return status();
}
OH_NN_ReturnCode OH_NNTensorDesc_SetDataType(NN_TensorDesc*, OH_NN_DataType) {
  return status();
}
OH_NN_ReturnCode OH_NNTensorDesc_SetFormat(NN_TensorDesc*, OH_NN_Format) {
  return status();
}
OH_NNModel* OH_NNModel_Construct() {
  return create<OH_NNModel>();
}
void OH_NNModel_Destroy(OH_NNModel** p) {
  destroy(p);
}
OH_NN_ReturnCode OH_NNModel_AddTensorToModel(OH_NNModel*,
                                             const NN_TensorDesc*) {
  return status();
}
OH_NN_ReturnCode OH_NNModel_SetTensorType(OH_NNModel*,
                                          uint32_t,
                                          OH_NN_TensorType) {
  return status();
}
OH_NN_ReturnCode OH_NNModel_SetTensorData(OH_NNModel*,
                                          uint32_t,
                                          const void*,
                                          size_t) {
  return status();
}
OH_NN_ReturnCode OH_NNModel_AddOperation(OH_NNModel*,
                                         OH_NN_OperationType op,
                                         const OH_NN_UInt32Array*,
                                         const OH_NN_UInt32Array* in,
                                         const OH_NN_UInt32Array* out) {
  assert(op == OH_NN_OPS_ADD && in->size == 2 && out->size == 1);
  return status();
}
OH_NN_ReturnCode OH_NNModel_SpecifyInputsAndOutputs(OH_NNModel*,
                                                    const OH_NN_UInt32Array*,
                                                    const OH_NN_UInt32Array*) {
  return status();
}
OH_NN_ReturnCode OH_NNModel_Finish(OH_NNModel*) {
  return status();
}
OH_NN_ReturnCode OH_NNModel_GetAvailableOperations(OH_NNModel*,
                                                   size_t id,
                                                   const bool** s,
                                                   uint32_t* n) {
  assert(id == 20);
  *s = &supported;
  *n = 1;
  return status();
}
OH_NNCompilation* OH_NNCompilation_Construct(const OH_NNModel*) {
  return create<OH_NNCompilation>();
}
void OH_NNCompilation_Destroy(OH_NNCompilation** p) {
  destroy(p);
}
OH_NN_ReturnCode OH_NNCompilation_SetDevice(OH_NNCompilation*, size_t id) {
  chosen = id;
  return status();
}
OH_NN_ReturnCode OH_NNCompilation_EnableFloat16(OH_NNCompilation*,
                                                bool enable) {
  assert(!enable);
  return status();
}
OH_NN_ReturnCode OH_NNCompilation_Build(OH_NNCompilation*) {
  return status();
}
OH_NNExecutor* OH_NNExecutor_Construct(OH_NNCompilation*) {
  return create<OH_NNExecutor>();
}
void OH_NNExecutor_Destroy(OH_NNExecutor** p) {
  destroy(p);
}
NN_Tensor* OH_NNTensor_Create(size_t id, NN_TensorDesc* d) {
  assert(id == 20);
  auto* p = create<NN_Tensor>();
  if (p)
    p->data.resize(d->bytes / 4);
  return p;
}
OH_NN_ReturnCode OH_NNTensor_Destroy(NN_Tensor** p) {
  destroy(p);
  return OH_NN_SUCCESS;
}
void* OH_NNTensor_GetDataBuffer(const NN_Tensor* p) {
  return fail() ? nullptr : const_cast<float*>(p->data.data());
}
OH_NN_ReturnCode OH_NNTensor_GetSize(const NN_Tensor* p, size_t* size) {
  *size = small_buffer ? 1 : p->data.size() * 4;
  return status();
}
OH_NN_ReturnCode OH_NNTensor_GetOffset(const NN_Tensor*, size_t* offset) {
  *offset = 0;
  return status();
}
OH_NN_ReturnCode OH_NNExecutor_RunSync(OH_NNExecutor*,
                                       NN_Tensor* in[],
                                       size_t ni,
                                       NN_Tensor* out[],
                                       size_t no) {
  assert(ni == 2 && no == 1);
  auto r = status();
  if (r != OH_NN_SUCCESS)
    return r;
  for (size_t i = 0; i < out[0]->data.size(); ++i)
    out[0]->data[i] = in[0]->data[i] + in[1]->data[i];
  return OH_NN_SUCCESS;
}
}
int main() {
  using webnn::ohos::NnrtAddModel;
  webnn::ohos::NnrtDevice device;
  std::string error;
  size_t bytes;
  for (const auto& shape : std::vector<std::vector<uint32_t>>{
           {}, {0}, {1, 1, 1, 1, 1}, {0xffffffff}, {1024, 1025}}) {
    assert(!NnrtAddModel::ValidateShape(shape, bytes, error));
  }
  assert(NnrtAddModel::ValidateShape({1024, 1024}, bytes, error) &&
         bytes == 4 * 1024 * 1024);
  assert(webnn::ohos::FindNnrtAccelerator(device, error) && device.id == 20);
  accelerator = false;
  assert(!webnn::ohos::FindNnrtAccelerator(device, error));
  assert(!NnrtAddModel::Create(device, {4}, error));
  accelerator = true;
  calls = 0;
  auto model = NnrtAddModel::Create(device, {4}, error);
  assert(model && chosen == 20);
  const int creation_calls = calls;
  const std::array<float, 4> lhs{1, -2, 0.25f, 1024},
      rhs{3, 0.5f, -0.125f, -512};
  std::array<float, 4> out{};
  calls = 0;
  assert(model->Run(lhs.data(), sizeof(lhs), rhs.data(), sizeof(rhs),
                    out.data(), sizeof(out), error));
  assert((out == std::array<float, 4>{4, -1.5f, 0.125f, 512}));
  const int run_calls = calls;
  for (int i = 1; i <= run_calls; ++i) {
    calls = 0;
    fail_at = i;
    out.fill(123);
    assert(!model->Run(lhs.data(), sizeof(lhs), rhs.data(), sizeof(rhs),
                       out.data(), sizeof(out), error));
    for (float value : out)
      assert(value == 123);
  }
  fail_at = -1;
  calls = 0;
  assert(!model->Run(lhs.data(), 4, rhs.data(), sizeof(rhs), out.data(),
                     sizeof(out), error) &&
         calls == 0);
  small_buffer = true;
  assert(!model->Run(lhs.data(), sizeof(lhs), rhs.data(), sizeof(rhs),
                     out.data(), sizeof(out), error));
  small_buffer = false;
  model.reset();
  assert(live == 0);
  for (int i = 1; i <= creation_calls; ++i) {
    calls = 0;
    fail_at = i;
    assert(!NnrtAddModel::Create(device, {4}, error));
    assert(live == 0);
  }
  fail_at = -1;
  supported = false;
  assert(!NnrtAddModel::Create(device, {4}, error) && live == 0);
  std::cout << "PASS: selection, shape bounds, numerical result, no fallback, "
            << creation_calls << " creation + " << run_calls
            << " execution failure points; all handles released\n";
}
