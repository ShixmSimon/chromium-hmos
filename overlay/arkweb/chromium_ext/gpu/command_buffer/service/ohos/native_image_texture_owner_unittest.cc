/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
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

#include "base/memory/ref_counted.h"
#define private public
#include "gpu/command_buffer/service/ohos/native_image_texture_owner.h"
#undef private

#include <gmock/gmock.h>

#include <cstddef>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/test_simple_task_runner.h"
#include "gpu/command_buffer/service/abstract_texture_ohos.h"
#include "gpu/command_buffer/service/decoder_context.h"
#include "gpu/command_buffer/service/ohos/scoped_native_buffer_fence_sync.h"
#include "gpu/command_buffer/service/texture_base.h"
#include "gtest/gtest.h"
#include "ui/gl/gpu_timing.h"
#include "ui/gl/scoped_make_current.h"

namespace gpu {
class MOCKNativeImageTextureOwner : public NativeImageTextureOwner {
 public:
  MOCKNativeImageTextureOwner(bool binds_texture_on_update,
                              std::unique_ptr<AbstractTextureOHOS> texture,
                              scoped_refptr<SharedContextState> context_state)
      : NativeImageTextureOwner(binds_texture_on_update,
                                std::move(texture),
                                std::move(context_state)) {}
  MOCKNativeImageTextureOwner(bool binds_texture_on_update,
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
  static int num;
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
};

int MockSharedContextState::num = 0;

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

class NativeImageTextureOwnerTest : public testing::Test {
 protected:
  void SetUp() override {
    share_group = new gl::GLShareGroup();
    share_group1 = new gl::GLShareGroup();
    context = new MockGLContext(share_group1);
    scoped_refptr<base::SingleThreadTaskRunner> task_runner(
        base::MakeRefCounted<base::TestSimpleTaskRunner>());
    base::SingleThreadTaskRunner::CurrentDefaultHandle sttcd1(task_runner);
    context_state = new SharedContextState(
        share_group, surface, context, use_virtualized_gl_contexts,
        std::move(context_lost_callback), gr_context_type,
        vulkan_context_provider, metal_context_provider, dawn_context_provider,
        peak_memory_monitor, created_on_compositor_gpu_thread);
    native_ptr = new MOCKNativeImageTextureOwner(
        binds_texture_on_update, std::move(texture), context_state);
  }
  void TearDown() override {}
  bool binds_texture_on_update = true;
  unsigned int service_id = 0;
  std::unique_ptr<TextureBase> texture1 =
      std::make_unique<TextureBase>(service_id);
  std::unique_ptr<AbstractTextureOHOS> texture =
      std::make_unique<AbstractTextureOHOS>(std::move(texture1));
  scoped_refptr<gl::GLShareGroup> share_group;
  scoped_refptr<gl::GLSurface> surface;
  scoped_refptr<MockGLContext> context;
  bool use_virtualized_gl_contexts = false;
  GrContextType gr_context_type = GrContextType::kGL;
  gpu::VulkanContextProvider* vulkan_context_provider = nullptr;
  viz::MetalContextProvider* metal_context_provider = nullptr;
  gpu::DawnContextProvider* dawn_context_provider = nullptr;
  base::WeakPtr<gpu::MemoryTracker::Observer> peak_memory_monitor = nullptr;
  gl::GLShareGroup* share_group_1 = new gl::GLShareGroup();
  bool created_on_compositor_gpu_thread = false;
  gl::GLShareGroup* share_group1;
  scoped_refptr<SharedContextState> context_state;
  scoped_refptr<NativeImageTextureOwner> native_ptr;
  SharedContextState::ContextLostCallback context_lost_callback =
      base::BindOnce([](bool lost, error::ContextLostReason reason) {});
};

TEST_F(NativeImageTextureOwnerTest, NativeImageTextureOwner0) {
  bool binds_texture_on_update = true;
  unsigned int service_id = 0;
  std::unique_ptr<TextureBase> texture1 =
      std::make_unique<TextureBase>(service_id);
  std::unique_ptr<AbstractTextureOHOS> texture =
      std::make_unique<AbstractTextureOHOS>(std::move(texture1));
  scoped_refptr<base::SingleThreadTaskRunner> task_runner(
      base::MakeRefCounted<base::TestSimpleTaskRunner>());
  base::SingleThreadTaskRunner::CurrentDefaultHandle sttcd1(task_runner);
  auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  scoped_refptr<MOCKNativeImageTextureOwner> mock_native_ptr =
      new MOCKNativeImageTextureOwner(binds_texture_on_update,
                                      std::move(texture), context_state);
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  EXPECT_CALL(*context, MakeCurrentImpl(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  EXPECT_CALL(*context, ReleaseCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  mock_native_ptr->context_state_ = new MockSharedContextState(
      share_group, surface, context, use_virtualized_gl_contexts,
      std::move(context_lost_callback), gr_context_type,
      vulkan_context_provider, metal_context_provider, dawn_context_provider,
      peak_memory_monitor, created_on_compositor_gpu_thread);
  mock_native_ptr.reset();
  EXPECT_EQ(MockSharedContextState::num, 1);
}

TEST_F(NativeImageTextureOwnerTest, NativeImageTextureOwner1) {
  bool binds_texture_on_update = true;
  unsigned int service_id = 0;
  std::unique_ptr<TextureBase> texture1 =
      std::make_unique<TextureBase>(service_id);
  std::unique_ptr<AbstractTextureOHOS> texture =
      std::make_unique<AbstractTextureOHOS>(std::move(texture1));
  scoped_refptr<base::SingleThreadTaskRunner> task_runner(
      base::MakeRefCounted<base::TestSimpleTaskRunner>());
  base::SingleThreadTaskRunner::CurrentDefaultHandle sttcd1(task_runner);
  auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  scoped_refptr<MOCKNativeImageTextureOwner> mock_native_ptr =
      new MOCKNativeImageTextureOwner(binds_texture_on_update,
                                      std::move(texture), context_state);
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  EXPECT_CALL(*context, ReleaseCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, MakeCurrentImpl(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  mock_native_ptr->context_state_ = new MockSharedContextState(
      share_group, surface, context, use_virtualized_gl_contexts,
      std::move(context_lost_callback), gr_context_type,
      vulkan_context_provider, metal_context_provider, dawn_context_provider,
      peak_memory_monitor, created_on_compositor_gpu_thread);
  mock_native_ptr->context_state_ = nullptr;
  mock_native_ptr.reset();
  EXPECT_EQ(MockSharedContextState::num, 2);
}

}  // namespace gpu
