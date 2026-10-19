// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_WEBNN_OHOS_CONTEXT_IMPL_OHOS_H_
#define SERVICES_WEBNN_OHOS_CONTEXT_IMPL_OHOS_H_

#include "base/feature_list.h"
#include "services/webnn/ohos/nnrt_add_model.h"
#include "services/webnn/webnn_context_impl.h"

namespace webnn::ohos {

BASE_DECLARE_FEATURE(kOhosWebNN);

class ContextImplOhos final : public WebNNContextImpl {
 public:
  // All contexts share one dedicated NNRt thread, including native teardown.
  static scoped_refptr<base::SingleThreadTaskRunner> GetNnrtTaskRunner();
  static WebNNContextImplPtr Create(
      mojo::PendingReceiver<mojom::WebNNContext> receiver,
      base::WeakPtr<WebNNContextProviderImpl> context_provider,
      mojom::CreateContextOptionsPtr options,
      std::unique_ptr<GpuTaskScheduler> gpu_task_scheduler,
      scoped_refptr<gpu::MemoryTracker> memory_tracker,
      scoped_refptr<base::SingleThreadTaskRunner> owning_task_runner,
      gpu::SharedImageManager* shared_image_manager,
      scoped_refptr<base::SingleThreadTaskRunner> main_task_runner);

  ~ContextImplOhos() override;
  void FailExecution(const std::string& reason);
  bool execution_failed() const { return execution_failed_; }
  base::WeakPtr<WebNNContextImpl> AsWeakPtr() override;
  void CreateGraphImpl(
      mojom::GraphInfoPtr graph_info,
      WebNNGraphImpl::ComputeResourceInfo compute_resource_info,
      base::flat_map<OperandId, std::unique_ptr<WebNNConstantOperand>>
          constants,
      CreateGraphImplCallback callback) override;
  std::string_view GetBackendName() const override;
  std::vector<mojom::WebNNExecutionProviderDetailsPtr>
  GetExecutionProvidersInfo() const override;

 private:
  ContextImplOhos(
      mojo::PendingReceiver<mojom::WebNNContext> receiver,
      base::WeakPtr<WebNNContextProviderImpl> context_provider,
      mojom::CreateContextOptionsPtr options,
      std::unique_ptr<GpuTaskScheduler> gpu_task_scheduler,
      scoped_refptr<gpu::MemoryTracker> memory_tracker,
      scoped_refptr<base::SingleThreadTaskRunner> owning_task_runner,
      gpu::SharedImageManager* shared_image_manager,
      scoped_refptr<base::SingleThreadTaskRunner> main_task_runner,
      NnrtDevice device);
  base::expected<scoped_refptr<WebNNTensorImpl>, mojom::ErrorPtr>
  CreateTensorImpl(mojo::PendingAssociatedReceiver<mojom::WebNNTensor> receiver,
                   mojom::TensorInfoPtr tensor_info) override;
  base::expected<scoped_refptr<WebNNTensorImpl>, mojom::ErrorPtr>
  CreateTensorFromSharedImageImpl(
      mojo::PendingAssociatedReceiver<mojom::WebNNTensor> receiver,
      mojom::TensorInfoPtr tensor_info,
      WebNNTensorImpl::RepresentationPtr representation) override;

  const NnrtDevice device_;
  bool execution_failed_ = false;
  base::WeakPtrFactory<ContextImplOhos> weak_factory_{this};
};

}  // namespace webnn::ohos
#endif  // SERVICES_WEBNN_OHOS_CONTEXT_IMPL_OHOS_H_
