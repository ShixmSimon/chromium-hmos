/*
 * Copyright (c) 2025 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

#include "base/functional/bind.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/metrics/histogram_macros.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/test/test_simple_task_runner.h"
#include "gpu/command_buffer/service/abstract_texture_ohos.h"
#include "gpu/command_buffer/service/decoder_context.h"
#include "gpu/command_buffer/service/ohos/scoped_native_buffer_fence_sync.h"
#include "gpu/command_buffer/service/texture_base.h"
#include "gtest/gtest.h"
#include "ui/gl/gpu_timing.h"
#include "ui/gl/scoped_make_current.h"

#define private public
#include "media/gpu/ohos/codec_buffer_wait_coordinator.h"
#undef private

using namespace testing;
using namespace OHOS::NWeb;

namespace gpu {
class MockTextureOwner : public NativeImageTextureOwner {
 public:
  MockTextureOwner(bool binds_texture_on_update,
                   std::unique_ptr<AbstractTextureOHOS> texture,
                   scoped_refptr<SharedContextState> context_state)
      : NativeImageTextureOwner(binds_texture_on_update,
                                std::move(texture),
                                std::move(context_state)) {}
  MockTextureOwner(bool binds_texture_on_update,
                   std::unique_ptr<AbstractTextureOHOS> texture)
      : NativeImageTextureOwner(binds_texture_on_update, std::move(texture)) {}
  MOCK_METHOD(gl::GLContext*, GetContext, (), (const));
  MOCK_METHOD(gl::GLSurface*, GetSurface, (), (const));
  MOCK_METHOD(void*, AquireOhosNativeWindow, (), (const));
  MOCK_METHOD(void, UpdateNativeImage, (), (override));
  MOCK_METHOD(void, GetSurfaceId, (uint64_t*), (override));
  MOCK_METHOD(void, EnsureNativeImageBound, (GLuint), (override));
  MOCK_METHOD(void, ReleaseNativeImage, (), (override));
  MOCK_METHOD(bool,
              GetCodedSizeAndVisibleRect,
              (gfx::Size, gfx::Size*, gfx::Rect*),
              (override));
  MOCK_METHOD(void,
              SetFrameAvailableCallback,
              (const base::RepeatingClosure&),
              (override));
  MOCK_METHOD(void, RunWhenBufferIsAvailable, (base::OnceClosure), (override));
  MOCK_METHOD(void, ReleaseResources, (), (override));
  MOCK_METHOD(std::unique_ptr<ScopedNativeBufferFenceSync>,
              GetNativeBuffer,
              (),
              (override));
};

class MockSharedContextState : public SharedContextState {
 public:
  MockSharedContextState(
      scoped_refptr<gl::GLShareGroup> share_group,
      scoped_refptr<gl::GLSurface> surface,
      scoped_refptr<gl::GLContext> context,
      bool use_virtualized_gl_contexts,
      ContextLostCallback context_lost_callback,
      GrContextType gr_context_type = GrContextType::kGL,
      gpu::VulkanContextProvider* vulkan_context_provider = nullptr,
      viz::MetalContextProvider* metal_context_provider = nullptr,
      gpu::DawnContextProvider* dawn_context_provider = nullptr,
      base::WeakPtr<gpu::MemoryTracker::Observer> peak_memory_monitor = nullptr,
      bool created_on_compositor_gpu_thread = false)
      : SharedContextState(share_group,
                           surface,
                           context,
                           use_virtualized_gl_contexts,
                           std::move(context_lost_callback),
                           gr_context_type,
                           vulkan_context_provider,
                           metal_context_provider,
                           dawn_context_provider,
                           peak_memory_monitor,
                           created_on_compositor_gpu_thread) {}
  ~MockSharedContextState() { num++; }

 private:
  int num = 0;
};

class MockGLContext : public gl::GLContext {
 public:
  explicit MockGLContext(gl::GLShareGroup* share_group)
      : gl::GLContext(share_group) {}
  MOCK_METHOD(scoped_refptr<gl::GPUTimingClient>,
              CreateGPUTimingClient,
              (),
              (override));
  MOCK_METHOD(bool,
              InitializeImpl,
              (gl::GLSurface * compatible_surface,
               const gl::GLContextAttribs& attribs),
              (override));
  MOCK_METHOD(void, ReleaseCurrent, (gl::GLSurface * surface), (override));
  MOCK_METHOD(bool, IsCurrent, (gl::GLSurface * surface), (override));
  MOCK_METHOD(void*, GetHandle, (), (override));
  MOCK_METHOD(const gfx::ExtensionSet&, GetExtensions, (), (override));
  MOCK_METHOD(bool, MakeCurrentImpl, (gl::GLSurface * surface), (override));
  MOCK_METHOD(void, ResetExtensions, (), (override));
};

}  // namespace gpu

namespace media {
class CodecBufferWaitCoordinatorTest : public testing::Test {
 protected:
  void SetUp() override {
    task_runner_ = base::SingleThreadTaskRunner::GetCurrentDefault();
    bool binds_texture_on_update = true;
    unsigned int service_id = 0;
    std::unique_ptr<gpu::TextureBase> texture1 =
        std::make_unique<gpu::TextureBase>(service_id);
    std::unique_ptr<gpu::AbstractTextureOHOS> texture =
        std::make_unique<gpu::AbstractTextureOHOS>(std::move(texture1));
    scoped_refptr<gl::GLShareGroup> share_group = new gl::GLShareGroup();
    gl::GLShareGroup* share_group1 = new gl::GLShareGroup();
    scoped_refptr<gpu::MockGLContext> context =
        new gpu::MockGLContext(share_group1);
    EXPECT_CALL(*context, IsCurrent(testing::_))
        .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*context, MakeCurrentImpl(testing::_))
        .WillRepeatedly(testing::Return(false));
    EXPECT_CALL(*context, ReleaseCurrent(testing::_))
        .WillRepeatedly(testing::Return());
    gpu::SharedContextState::ContextLostCallback context_lost_callback =
        base::BindOnce([](bool lost, gpu::error::ContextLostReason reason) {});
    scoped_refptr<gl::GLSurface> surface;
    bool use_virtualized_gl_contexts = false;
    gpu::GrContextType gr_context_type = gpu::GrContextType::kGL;
    gpu::VulkanContextProvider* vulkan_context_provider = nullptr;
    viz::MetalContextProvider* metal_context_provider = nullptr;
    gpu::DawnContextProvider* dawn_context_provider = nullptr;
    base::WeakPtr<gpu::MemoryTracker::Observer> peak_memory_monitor = nullptr;
    bool created_on_compositor_gpu_thread = false;
    scoped_refptr<gpu::SharedContextState> context_state =
        new gpu::SharedContextState(
            share_group, surface, context, use_virtualized_gl_contexts,
            std::move(context_lost_callback), gr_context_type,
            vulkan_context_provider, metal_context_provider,
            dawn_context_provider, peak_memory_monitor,
            created_on_compositor_gpu_thread);
    texture_owner_ = new gpu::MockTextureOwner(
        binds_texture_on_update, std::move(texture), context_state);

    drdc_lock_ = base::MakeRefCounted<gpu::RefCountedLock>();

    EXPECT_CALL(*texture_owner_, SetFrameAvailableCallback(_))
        .WillOnce([this](const base::RepeatingClosure& callback) {
          frame_available_callback_ = callback;
        });

    coordinator_ = base::MakeRefCounted<CodecBufferWaitCoordinator>(
        std::move(texture_owner_), drdc_lock_);
    EXPECT_NE(coordinator_, nullptr);
  }

  void TearDown() override {
    frame_available_callback_.Reset();
    drdc_lock_.reset();
  }

  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
  base::test::TaskEnvironment task_environment_;
  scoped_refptr<gpu::MockTextureOwner> texture_owner_;
  scoped_refptr<gpu::RefCountedLock> drdc_lock_;
  scoped_refptr<CodecBufferWaitCoordinator> coordinator_;
  base::RepeatingClosure frame_available_callback_;
};

TEST_F(CodecBufferWaitCoordinatorTest, SetReleaseTimeToNow) {
  coordinator_->SetReleaseTimeToNow();
  EXPECT_TRUE(coordinator_->IsExpectingFrameAvailable());
}

TEST_F(CodecBufferWaitCoordinatorTest, IsExpectingFrameAvailable) {
  EXPECT_FALSE(coordinator_->IsExpectingFrameAvailable());
  coordinator_->SetReleaseTimeToNow();
  EXPECT_TRUE(coordinator_->IsExpectingFrameAvailable());
}

TEST_F(CodecBufferWaitCoordinatorTest, WaitForFrameAvailable_Signaled) {
  coordinator_->SetReleaseTimeToNow();
  frame_available_callback_.Run();
  coordinator_->WaitForFrameAvailable();
  EXPECT_FALSE(coordinator_->IsExpectingFrameAvailable());
}

TEST_F(CodecBufferWaitCoordinatorTest,
       WaitForFrameAvailable_SuccessDuringWait) {
  base::AutoLock lock(*drdc_lock_->GetDrDcLockPtr());
  coordinator_->SetReleaseTimeToNow();
  base::WaitableEvent signal_event;
  base::ThreadPool::PostDelayedTask(
      FROM_HERE, {base::TaskPriority::BEST_EFFORT},
      base::BindOnce(
          [](base::RepeatingClosure callback, base::WaitableEvent* event) {
            callback.Run();
            event->Signal();
          },
          frame_available_callback_, &signal_event),
      base::Milliseconds(1));

  coordinator_->WaitForFrameAvailable();
  EXPECT_FALSE(coordinator_->IsExpectingFrameAvailable());
}

TEST_F(CodecBufferWaitCoordinatorTest,
       WaitForFrameAvailable_TimeoutDuringWait) {
  coordinator_->SetReleaseTimeToNow();
  coordinator_->WaitForFrameAvailable();
  EXPECT_FALSE(coordinator_->IsExpectingFrameAvailable());
}

TEST_F(CodecBufferWaitCoordinatorTest,
       WaitForFrameAvailable_AlreadyTimedOut_NoSignal) {
  coordinator_->SetReleaseTimeToNow();
  base::PlatformThread::Sleep(base::Milliseconds(5));
  coordinator_->WaitForFrameAvailable();
  EXPECT_FALSE(coordinator_->IsExpectingFrameAvailable());
}

TEST_F(CodecBufferWaitCoordinatorTest,
       WaitForFrameAvailable_AlreadyTimedOut_WithSignal) {
  coordinator_->SetReleaseTimeToNow();
  frame_available_callback_.Run();
  base::PlatformThread::Sleep(base::Milliseconds(5));
  coordinator_->WaitForFrameAvailable();
  EXPECT_FALSE(coordinator_->IsExpectingFrameAvailable());
}
}  // namespace media
