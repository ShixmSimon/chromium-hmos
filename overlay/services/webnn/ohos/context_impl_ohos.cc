// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/webnn/ohos/context_impl_ohos.h"

#include "base/containers/heap_array.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/notreached.h"
#include "base/task/thread_pool.h"
#include "base/trace_event/trace_event.h"
#include "services/webnn/gpu_task_scheduler.h"
#include "services/webnn/public/mojom/webnn_error.mojom.h"
#include "services/webnn/public/mojom/webnn_graph.mojom.h"
#include "services/webnn/public/mojom/webnn_service_introspection.mojom.h"

namespace webnn::ohos {

BASE_FEATURE(kOhosWebNN, base::FEATURE_DISABLED_BY_DEFAULT);

namespace {

ContextProperties GetProperties() {
  const SupportedTensors fp32{{OperandDataType::kFloat32},
                              SupportedRanks::NonScalarUpTo(4)};
  // Construct every limit explicitly: unsupported operations have empty sets.
  // Mojo's deserialization-only constructor must not be used here.
  DataTypeLimits limits(
      /*input=*/fp32,
      /*constant=*/{},
      /*arg_min_max_input=*/{},
      /*arg_min_max_output=*/{},
      /*batch_normalization_input=*/{},
      /*batch_normalization_mean=*/{},
      /*cast_input=*/{},
      /*clamp_input=*/{},
      /*concat_inputs=*/{},
      /*conv2d_input=*/{},
      /*conv2d_bias=*/{},
      /*conv_transpose2d_input=*/{},
      /*conv_transpose2d_bias=*/{},
      /*cumulative_sum_input=*/{},
      /*dequantize_linear_input=*/{},
      /*dequantize_linear_scale=*/{},
      /*add_input=*/fp32,
      /*sub_input=*/{},
      /*mul_input=*/{},
      /*div_input=*/{},
      /*max_input=*/{},
      /*min_input=*/{},
      /*pow_input=*/{},
      /*equal_input=*/{},
      /*greater_input=*/{},
      /*greater_or_equal_input=*/{},
      /*lesser_input=*/{},
      /*lesser_or_equal_input=*/{},
      /*not_equal_input=*/{},
      /*logical_and_input=*/{},
      /*logical_or_input=*/{},
      /*logical_xor_input=*/{},
      /*logical_not_input=*/{},
      /*is_nan_input=*/{},
      /*is_infinite_input=*/{},
      /*logical_output=*/{},
      /*abs_input=*/{},
      /*ceil_input=*/{},
      /*cos_input=*/{},
      /*erf_input=*/{},
      /*exp_input=*/{},
      /*floor_input=*/{},
      /*identity_input=*/{},
      /*log_input=*/{},
      /*neg_input=*/{},
      /*reciprocal_input=*/{},
      /*round_even_input=*/{},
      /*sign_input=*/{},
      /*sin_input=*/{},
      /*sqrt_input=*/{},
      /*tan_input=*/{},
      /*elu_input=*/{},
      /*expand_input=*/{},
      /*gather_input=*/{},
      /*gather_indices=*/{},
      /*gather_elements_input=*/{},
      /*gather_elements_indices=*/{},
      /*gather_nd_input=*/{},
      /*gather_nd_indices=*/{},
      /*gelu_input=*/{},
      /*gemm_a=*/{},
      /*gemm_c=*/{},
      /*gru_input=*/{},
      /*gru_bias=*/{},
      /*gru_output_sequence=*/{},
      /*gru_cell_input=*/{},
      /*gru_cell_bias=*/{},
      /*hard_sigmoid_input=*/{},
      /*hard_swish_input=*/{},
      /*instance_normalization_input=*/{},
      /*instance_normalization_scale=*/{},
      /*layer_normalization_input=*/{},
      /*leaky_relu_input=*/{},
      /*linear_input=*/{},
      /*lstm_input=*/{},
      /*lstm_bias=*/{},
      /*lstm_output_sequence=*/{},
      /*lstm_cell_input=*/{},
      /*lstm_cell_bias=*/{},
      /*matmul_input=*/{},
      /*pad_input=*/{},
      /*average_pool2d_input=*/{},
      /*l2_pool2d_input=*/{},
      /*max_pool2d_input=*/{},
      /*prelu_input=*/{},
      /*quantize_linear_input=*/{},
      /*quantize_linear_zero_point=*/{},
      /*reduce_l1_input=*/{},
      /*reduce_l2_input=*/{},
      /*reduce_log_sum_input=*/{},
      /*reduce_log_sum_exp_input=*/{},
      /*reduce_max_input=*/{},
      /*reduce_mean_input=*/{},
      /*reduce_min_input=*/{},
      /*reduce_product_input=*/{},
      /*reduce_sum_input=*/{},
      /*reduce_sum_square_input=*/{},
      /*relu_input=*/{},
      /*resample2d_input=*/{},
      /*reshape_input=*/{},
      /*reverse_input=*/{},
      /*scatter_elements_input=*/{},
      /*scatter_elements_indices=*/{},
      /*scatter_nd_input=*/{},
      /*scatter_nd_indices=*/{},
      /*scatter_nd_updates=*/{},
      /*sigmoid_input=*/{},
      /*slice_input=*/{},
      /*softmax_input=*/{},
      /*softplus_input=*/{},
      /*softsign_input=*/{},
      /*split_input=*/{},
      /*tanh_input=*/{},
      /*tile_input=*/{},
      /*transpose_input=*/{},
      /*triangular_input=*/{},
      /*where_condition=*/{},
      /*where_value=*/{});
  return ContextProperties(InputOperandLayout::kNchw, Resample2DAxes::kAny,
                           BatchNormalizationAxis::kAny,
                           NnrtAddModel::kMaxTensorBytes, std::move(limits));
}

mojom::ErrorPtr NotSupported(const std::string& message) {
  return mojom::Error::New(mojom::Error::Code::kNotSupportedError, message);
}

class TensorImplOhos final : public WebNNTensorImpl {
 public:
  TensorImplOhos(mojo::PendingAssociatedReceiver<mojom::WebNNTensor> receiver,
                 WebNNContextImpl& context,
                 mojom::TensorInfoPtr info)
      : WebNNTensorImpl(std::move(receiver), context, std::move(info)),
        data_(base::HeapArray<uint8_t>::WithSize(PackedByteLength())) {}

  base::span<uint8_t> data() { return data_.as_span(); }
  void WriteTensorImpl(mojo_base::BigBuffer buffer) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (!static_cast<ContextImplOhos&>(*context_).execution_failed()) {
      context_->ReadDataFromBigBufferOrDataPipe(std::move(buffer), data());
    }
  }

 private:
  ~TensorImplOhos() override = default;
  void ReadTensorImpl(ReadTensorCallback callback) override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    if (static_cast<ContextImplOhos&>(*context_).execution_failed()) {
      std::move(callback).Run(mojom::ReadTensorResult::NewError(
          mojom::Error::New(mojom::Error::Code::kUnknownError,
                            "NNRt context lost after execution failure")));
      return;
    }
    std::move(callback).Run(mojom::ReadTensorResult::NewBuffer(
        context_->WriteDataToDataPipeOrBigBuffer(data())));
  }
  bool ImportTensorImpl(ScopedAccessPtr access) override { return false; }
  void ExportTensorImpl(ScopedAccessPtr access) override { NOTREACHED(); }
  base::HeapArray<uint8_t> data_;
};

class GraphImplOhos final : public WebNNGraphImpl {
 public:
  GraphImplOhos(WebNNContextImpl& context,
                ComputeResourceInfo resources,
                std::unique_ptr<NnrtAddModel> model,
                std::string lhs,
                std::string rhs,
                std::string output)
      : WebNNGraphImpl(context, std::move(resources), {mojom::Device::kNpu}),
        model_(std::move(model)),
        lhs_(std::move(lhs)),
        rhs_(std::move(rhs)),
        output_(std::move(output)) {}

 private:
  ~GraphImplOhos() override = default;
  void DispatchImpl(
      base::flat_map<std::string, scoped_refptr<WebNNTensorImpl>> inputs,
      base::flat_map<std::string, scoped_refptr<WebNNTensorImpl>> outputs)
      override {
    if (static_cast<ContextImplOhos&>(*context_).execution_failed()) {
      return;
    }
    TRACE_EVENT("webnn", "OhosWebNNDispatch");
    // Context dispatch already validated ownership, types, shapes and names.
    // These synchronous calls and all tensor operations share the NNRt thread.
    const auto lhs =
        static_cast<TensorImplOhos*>(inputs.at(lhs_).get())->data();
    const auto rhs =
        static_cast<TensorImplOhos*>(inputs.at(rhs_).get())->data();
    auto output =
        static_cast<TensorImplOhos*>(outputs.at(output_).get())->data();
    std::string error;
    if (!model_->Run(lhs.data(), lhs.size(), rhs.data(), rhs.size(),
                     output.data(), output.size(), error)) {
      LOG(ERROR) << "OhosWebNN execution failed: " << error;
      // Never resolve a later read with a stale result after device failure.
      // OnLost schedules context removal; do not touch this graph afterwards.
      static_cast<ContextImplOhos&>(*context_).FailExecution(error);
      return;
    }
    TRACE_EVENT_INSTANT("webnn", "OhosWebNNExecutionCompleted", "tensor_bytes",
                        model_->byte_length());
  }
  std::unique_ptr<NnrtAddModel> model_;
  const std::string lhs_;
  const std::string rhs_;
  const std::string output_;
};

}  // namespace

scoped_refptr<base::SingleThreadTaskRunner>
ContextImplOhos::GetNnrtTaskRunner() {
  static const base::NoDestructor<scoped_refptr<base::SingleThreadTaskRunner>>
      runner(base::ThreadPool::CreateSingleThreadTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
          base::SingleThreadTaskRunnerThreadMode::DEDICATED));
  return *runner;
}

WebNNContextImpl::WebNNContextImplPtr ContextImplOhos::Create(
    mojo::PendingReceiver<mojom::WebNNContext> receiver,
    base::WeakPtr<WebNNContextProviderImpl> context_provider,
    mojom::CreateContextOptionsPtr options,
    std::unique_ptr<GpuTaskScheduler> gpu_task_scheduler,
    scoped_refptr<gpu::MemoryTracker> memory_tracker,
    scoped_refptr<base::SingleThreadTaskRunner> owning_task_runner,
    gpu::SharedImageManager* shared_image_manager,
    scoped_refptr<base::SingleThreadTaskRunner> main_task_runner) {
  CHECK(owning_task_runner->RunsTasksInCurrentSequence());
  NnrtDevice device;
  std::string error;
  if (options->device != mojom::Device::kNpu ||
      !FindNnrtAccelerator(device, error)) {
    LOG(ERROR) << "OhosWebNN unavailable: " << error;
    return WebNNContextImplPtr(nullptr,
                               OnTaskRunnerDeleter(owning_task_runner));
  }
  LOG(INFO) << "OhosWebNN selected accelerator id=" << device.id
            << " name=" << device.name;
  TRACE_EVENT_INSTANT("webnn", "OhosWebNNDeviceSelected", "device_id",
                      device.id);
  auto deleter = OnTaskRunnerDeleter(owning_task_runner);
  return WebNNContextImplPtr(
      new ContextImplOhos(std::move(receiver), std::move(context_provider),
                          std::move(options), std::move(gpu_task_scheduler),
                          std::move(memory_tracker),
                          std::move(owning_task_runner), shared_image_manager,
                          std::move(main_task_runner), std::move(device)),
      std::move(deleter));
}

ContextImplOhos::ContextImplOhos(
    mojo::PendingReceiver<mojom::WebNNContext> receiver,
    base::WeakPtr<WebNNContextProviderImpl> context_provider,
    mojom::CreateContextOptionsPtr options,
    std::unique_ptr<GpuTaskScheduler> gpu_task_scheduler,
    scoped_refptr<gpu::MemoryTracker> memory_tracker,
    scoped_refptr<base::SingleThreadTaskRunner> owning_task_runner,
    gpu::SharedImageManager* shared_image_manager,
    scoped_refptr<base::SingleThreadTaskRunner> main_task_runner,
    NnrtDevice device)
    : WebNNContextImpl(std::move(receiver),
                       std::move(context_provider),
                       ContextBackendUma::kOhosNnrt,
                       GetProperties(),
                       std::move(options),
                       mojo::ScopedDataPipeConsumerHandle(),
                       mojo::ScopedDataPipeProducerHandle(),
                       std::move(gpu_task_scheduler),
                       std::move(memory_tracker),
                       std::move(owning_task_runner),
                       shared_image_manager,
                       std::move(main_task_runner)),
      device_(std::move(device)) {}

ContextImplOhos::~ContextImplOhos() = default;
void ContextImplOhos::FailExecution(const std::string& reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  execution_failed_ = true;
  OnLost(reason);
}
base::WeakPtr<WebNNContextImpl> ContextImplOhos::AsWeakPtr() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return weak_factory_.GetWeakPtr();
}

void ContextImplOhos::CreateGraphImpl(
    mojom::GraphInfoPtr info,
    WebNNGraphImpl::ComputeResourceInfo resources,
    base::flat_map<OperandId, std::unique_ptr<WebNNConstantOperand>> constants,
    CreateGraphImplCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  TRACE_EVENT("webnn", "OhosWebNNCompile");
  if (!constants.empty() || info->operations.size() != 1 ||
      info->input_operands.size() != 2 || info->output_operands.size() != 1 ||
      !info->operations[0]->is_element_wise_binary()) {
    std::move(callback).Run(
        base::unexpected(NotSupported("OHOS prototype requires one Add, two "
                                      "inputs and one output; no constants")));
    return;
  }
  const auto& add = *info->operations[0]->get_element_wise_binary();
  if (add.kind != mojom::ElementWiseBinary::Kind::kAdd ||
      add.lhs_operand_id == add.rhs_operand_id ||
      add.output_operand_id != info->output_operands[0]) {
    std::move(callback).Run(base::unexpected(NotSupported(
        "OHOS prototype requires Add with two distinct input operands")));
    return;
  }
  const auto& lhs = *info->operands[add.lhs_operand_id.value()];
  const auto& rhs = *info->operands[add.rhs_operand_id.value()];
  const auto& output = *info->operands[add.output_operand_id.value()];
  if (lhs.kind != mojom::Operand::Kind::kInput ||
      rhs.kind != mojom::Operand::Kind::kInput || !lhs.name || !rhs.name ||
      !output.name || lhs.descriptor.data_type() != OperandDataType::kFloat32 ||
      rhs.descriptor.data_type() != OperandDataType::kFloat32 ||
      output.descriptor.data_type() != OperandDataType::kFloat32 ||
      lhs.descriptor.shape() != rhs.descriptor.shape() ||
      lhs.descriptor.shape() != output.descriptor.shape()) {
    std::move(callback).Run(base::unexpected(NotSupported(
        "OHOS prototype requires identical float32 shapes; no broadcasting")));
    return;
  }
  std::string error;
  auto model = NnrtAddModel::Create(device_, lhs.descriptor.shape(), error);
  if (!model) {
    LOG(ERROR) << "OhosWebNN build failed: " << error;
    std::move(callback).Run(base::unexpected(NotSupported(error)));
    return;
  }
  LOG(INFO) << "OhosWebNN compiled Add on accelerator id=" << device_.id
            << " tensor_bytes=" << model->byte_length();
  std::move(callback).Run(base::MakeRefCounted<GraphImplOhos>(
      *this, std::move(resources), std::move(model), *lhs.name, *rhs.name,
      *output.name));
}

base::expected<scoped_refptr<WebNNTensorImpl>, mojom::ErrorPtr>
ContextImplOhos::CreateTensorImpl(
    mojo::PendingAssociatedReceiver<mojom::WebNNTensor> receiver,
    mojom::TensorInfoPtr info) {
  size_t bytes = 0;
  std::string error;
  if (info->descriptor.data_type() != OperandDataType::kFloat32 ||
      !NnrtAddModel::ValidateShape(info->descriptor.shape(), bytes, error)) {
    return base::unexpected(NotSupported("Unsupported OHOS tensor: " + error));
  }
  return base::MakeRefCounted<TensorImplOhos>(std::move(receiver), *this,
                                              std::move(info));
}

base::expected<scoped_refptr<WebNNTensorImpl>, mojom::ErrorPtr>
ContextImplOhos::CreateTensorFromSharedImageImpl(
    mojo::PendingAssociatedReceiver<mojom::WebNNTensor> receiver,
    mojom::TensorInfoPtr info,
    WebNNTensorImpl::RepresentationPtr representation) {
  return base::unexpected(
      NotSupported("OHOS WebNN WebGPU interop is not supported"));
}

std::string_view ContextImplOhos::GetBackendName() const {
  return "OHOS NNRt";
}
std::vector<mojom::WebNNExecutionProviderDetailsPtr>
ContextImplOhos::GetExecutionProvidersInfo() const {
  return {};
}

}  // namespace webnn::ohos
