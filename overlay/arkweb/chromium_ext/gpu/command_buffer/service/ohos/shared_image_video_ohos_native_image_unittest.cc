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

#include "gpu/command_buffer/service/ohos/shared_image_video_ohos_native_image.cc"

#include <cstddef>
#include <memory>
#include <utility>

#include "base/memory/scoped_refptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/test_simple_task_runner.h"
#include "command_buffer/common/mailbox.h"
#include "command_buffer/service/shared_image/shared_image_manager.h"
#include "command_buffer/service/texture_base.h"
#include "gmock/gmock.h"
#include "gpu/command_buffer/service/abstract_texture_ohos.h"
#include "gpu/command_buffer/service/mailbox_manager.h"
#include "gpu/command_buffer/service/memory_tracking.h"
#include "gpu/command_buffer/service/ohos/native_image_texture_owner.h"
#include "gpu/command_buffer/service/ohos/shared_image_video_ohos_native_image.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/command_buffer/service/shared_image/shared_image_representation.h"
#include "gpu/command_buffer/service/skia_utils.h"
#include "gtest/gtest.h"
#include "include/core/SkAlphaType.h"
#include "include/gpu/GrTypes.h"
#include "ui/gl/gl_share_group.h"
#include "ui/gl/gpu_timing.h"

using ::testing::_;

namespace gpu {
using ContextLostCallback = base::OnceCallback<void(bool)>;
class MockStreamTextureSharedImageInterface
    : public StreamTextureSharedImageInterface {
 public:
  MOCK_METHOD(void, ReleaseResources, (), (override));
  MOCK_METHOD(bool, IsUsingGpuMemory, (), (const, override));
  MOCK_METHOD(void, UpdateAndBindTexImage, (GLuint service_id), (override));
  MOCK_METHOD(bool, HasTextureOwner, (), (const, override));
  MOCK_METHOD(TextureBase*, GetTextureBase, (), (const, override));
  MOCK_METHOD(void,
              NotifyOverlayPromotion,
              (bool promotion, const gfx::Rect& bounds),
              (override));
  MOCK_METHOD(bool, RenderToOverlay, (), (override));
  MOCK_METHOD(bool, TextureOwnerBindsTextureOnUpdate, (), (override));
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
              Initialize,
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

class SharedImageVideoOhosNativeImageTest : public testing::Test {
 protected:
  void SetUp() override {
    size = gfx::Size(10, 20);
    share_group = new gl::GLShareGroup();
    share_group1 = new gl::GLShareGroup();
    context = new MockGLContext(share_group1);
    stream_texture_sii = new MockStreamTextureSharedImageInterface();
    shared_context_state = new SharedContextState(
        share_group, surface, context, use_virtualized_gl_contexts,
        std::move(context_lost_callback), gr_context_type,
        vulkan_context_provider, metal_context_provider, dawn_context_provider,
        peak_memory_monitor, created_on_compositor_gpu_thread);
    scoped_refptr<base::SingleThreadTaskRunner> task_runner(
        base::MakeRefCounted<base::TestSimpleTaskRunner>());
    base::SingleThreadTaskRunner::CurrentDefaultHandle sttcd1(task_runner);
    share_image_ptr = std::make_unique<SharedImageVideoOhosNativeImage>(
        mailbox, size, color_space, surface_origin, alpha_type,
        stream_texture_sii, shared_context_state);
    manager = std::make_unique<SharedImageManager>(
        thread_safe, display_context_on_another_thread);
    tracker = std::make_unique<MemoryTypeTracker>(memory_tracker, task_runner);
    backing = std::make_unique<SharedImageVideoOhosNativeImage>(
        mailbox, size, color_space, surface_origin, alpha_type,
        stream_texture_sii, shared_context_state);
  }
  void TearDown() override { share_image_ptr.reset(); }
  SharedContextState::ContextLostCallback context_lost_callback =
      base::BindOnce([](bool lost) {});
  const Mailbox mailbox;
  gfx::Size size;
  const gfx::ColorSpace color_space;
  GrSurfaceOrigin surface_origin = GrSurfaceOrigin::kBottomLeft_GrSurfaceOrigin;
  SkAlphaType alpha_type = SkAlphaType::kLastEnum_SkAlphaType;
  scoped_refptr<MockStreamTextureSharedImageInterface> stream_texture_sii;
  scoped_refptr<SharedContextState> shared_context_state;
  scoped_refptr<gl::GLShareGroup> share_group;
  scoped_refptr<gl::GLSurface> surface;
  scoped_refptr<MockGLContext> context;
  bool use_virtualized_gl_contexts = false;
  GrContextType gr_context_type = GrContextType::kGL;
  gpu::VulkanContextProvider* vulkan_context_provider = nullptr;
  viz::MetalContextProvider* metal_context_provider = nullptr;
  viz::DawnContextProvider* dawn_context_provider = nullptr;
  base::WeakPtr<gpu::MemoryTracker::Observer> peak_memory_monitor = nullptr;
  gl::GLShareGroup* share_group_1 = new gl::GLShareGroup();
  bool created_on_compositor_gpu_thread = false;
  std::unique_ptr<SharedImageVideoOhosNativeImage> share_image_ptr = nullptr;
  gl::GLShareGroup* share_group1;
  bool thread_safe = false;
  bool display_context_on_another_thread = false;
  std::unique_ptr<MemoryTypeTracker> tracker = nullptr;
  std::unique_ptr<SharedImageManager> manager = nullptr;
  MemoryTracker* memory_tracker = nullptr;
  std::unique_ptr<SharedImageVideoOhosNativeImage> backing = nullptr;
};

TEST_F(SharedImageVideoOhosNativeImageTest,
       SharedImageVideoOhosNativeImageCreate) {
  scoped_refptr<base::SingleThreadTaskRunner> task_runner(
      base::MakeRefCounted<base::TestSimpleTaskRunner>());
  base::SingleThreadTaskRunner::CurrentDefaultHandle sttcd1(task_runner);
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  auto share_image_ptr1 = std::make_shared<SharedImageVideoOhosNativeImage>(
      mailbox, size, color_space, surface_origin, alpha_type,
      stream_texture_sii, shared_context_state);
  EXPECT_TRUE(share_image_ptr1);
}

TEST_F(SharedImageVideoOhosNativeImageTest,
       SharedImageVideoOhosNativeImageDelete0) {
  scoped_refptr<base::SingleThreadTaskRunner> task_runner(
      base::MakeRefCounted<base::TestSimpleTaskRunner>());
  base::SingleThreadTaskRunner::CurrentDefaultHandle sttcd1(task_runner);
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  auto share_image_ptr1 = std::make_shared<SharedImageVideoOhosNativeImage>(
      mailbox, size, color_space, surface_origin, alpha_type,
      stream_texture_sii, shared_context_state);
  share_image_ptr1.reset();
  EXPECT_FALSE(share_image_ptr1);
}

TEST_F(SharedImageVideoOhosNativeImageTest,
       SharedImageVideoOhosNativeImageDelete1) {
  scoped_refptr<base::SingleThreadTaskRunner> task_runner(
      base::MakeRefCounted<base::TestSimpleTaskRunner>());
  base::SingleThreadTaskRunner::CurrentDefaultHandle sttcd1(task_runner);
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  auto share_image_ptr1 = std::make_shared<SharedImageVideoOhosNativeImage>(
      mailbox, size, color_space, surface_origin, alpha_type,
      stream_texture_sii, shared_context_state);
  share_image_ptr1->context_state_ = nullptr;
  share_image_ptr1.reset();
  EXPECT_FALSE(share_image_ptr1);
}

TEST_F(SharedImageVideoOhosNativeImageTest, GetEstimatedSizeForMemoryDump) {
  EXPECT_CALL(*stream_texture_sii, IsUsingGpuMemory())
      .Times(1)
      .WillRepeatedly(testing::Return(true));
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(1)
      .WillRepeatedly(testing::Return(false));
  auto result = share_image_ptr->GetEstimatedSizeForMemoryDump();
  EXPECT_NE(result, false);
}

TEST_F(SharedImageVideoOhosNativeImageTest, OnContextLost) {
  EXPECT_CALL(*stream_texture_sii, IsUsingGpuMemory())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(true));
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  share_image_ptr->OnContextLost();
  EXPECT_FALSE(share_image_ptr->context_state_);
}

TEST_F(SharedImageVideoOhosNativeImageTest, ProduceGLTexture) {
  EXPECT_CALL(*stream_texture_sii, HasTextureOwner())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  auto result = share_image_ptr->ProduceGLTexture(manager.get(), tracker.get());
  EXPECT_FALSE(result);
}

TEST_F(SharedImageVideoOhosNativeImageTest, ProduceGLTexturePassthrough) {
  EXPECT_CALL(*stream_texture_sii, HasTextureOwner())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  auto result = share_image_ptr->ProduceGLTexturePassthrough(manager.get(),
                                                             tracker.get());
  EXPECT_FALSE(result);
}

TEST_F(SharedImageVideoOhosNativeImageTest, ProduceSkiaGanesh) {
  scoped_refptr<SharedContextState> context_state;
  EXPECT_CALL(*stream_texture_sii, HasTextureOwner())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  auto result = share_image_ptr->ProduceSkiaGanesh(manager.get(), tracker.get(),
                                                   context_state);
  EXPECT_FALSE(result);
}

TEST_F(SharedImageVideoOhosNativeImageTest, BeginGLReadAccess) {
  const GLuint service_id = 0;
  EXPECT_CALL(*stream_texture_sii, UpdateAndBindTexImage(0))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  share_image_ptr->BeginGLReadAccess(service_id);
}

TEST_F(SharedImageVideoOhosNativeImageTest, ProduceLegacyOverlay) {
  EXPECT_CALL(*stream_texture_sii, HasTextureOwner())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  share_image_ptr->ProduceLegacyOverlay(manager.get(), tracker.get());
}

TEST_F(SharedImageVideoOhosNativeImageTest, RenderToOverlay) {
  std::unique_ptr<
      SharedImageVideoOhosNativeImage::SharedImageRepresentationOverlayVideo>
      overlay_video_instance =
          std::make_unique<SharedImageVideoOhosNativeImage::
                               SharedImageRepresentationOverlayVideo>(
              manager.get(), backing.get(), tracker.get());
  EXPECT_CALL(*stream_texture_sii, RenderToOverlay())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  overlay_video_instance->RenderToOverlay();
}

TEST_F(SharedImageVideoOhosNativeImageTest, NotifyOverlayPromotion) {
  bool promotion = false;
  const gfx::Rect bounds;
  std::unique_ptr<
      SharedImageVideoOhosNativeImage::SharedImageRepresentationOverlayVideo>
      overlay_video_instance =
          std::make_unique<SharedImageVideoOhosNativeImage::
                               SharedImageRepresentationOverlayVideo>(
              manager.get(), backing.get(), tracker.get());
  EXPECT_CALL(*stream_texture_sii, NotifyOverlayPromotion(false, _))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  overlay_video_instance->NotifyOverlayPromotion(promotion, bounds);
}

TEST_F(SharedImageVideoOhosNativeImageTest, stream_image) {
  std::unique_ptr<
      SharedImageVideoOhosNativeImage::SharedImageRepresentationOverlayVideo>
      overlay_video_instance =
          std::make_unique<SharedImageVideoOhosNativeImage::
                               SharedImageRepresentationOverlayVideo>(
              manager.get(), backing.get(), tracker.get());
  EXPECT_CALL(*stream_texture_sii, NotifyOverlayPromotion(false, _))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  overlay_video_instance->stream_image();
}

TEST_F(SharedImageVideoOhosNativeImageTest,
       SharedImageRepresentationGLTextureVideoDelete0) {
  scoped_refptr<base::SingleThreadTaskRunner> task_runner(
      base::MakeRefCounted<base::TestSimpleTaskRunner>());
  base::SingleThreadTaskRunner::CurrentDefaultHandle sttcd1(task_runner);
  unsigned int service_id = 1;
  std::unique_ptr<TextureBase> texture1 =
      std::make_unique<TextureBase>(service_id);
  std::unique_ptr<AbstractTextureOHOS> texture =
      std::make_unique<AbstractTextureOHOS>(std::move(texture1));
  std::unique_ptr<
      SharedImageVideoOhosNativeImage::SharedImageRepresentationGLTextureVideo>
      shared_video_ptr =
          std::make_unique<SharedImageVideoOhosNativeImage::
                               SharedImageRepresentationGLTextureVideo>(
              manager.get(), backing.get(), tracker.get(), std::move(texture));
  EXPECT_CALL(*stream_texture_sii, NotifyOverlayPromotion(false, _))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*stream_texture_sii, ReleaseResources())
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return());
  EXPECT_CALL(*context, IsCurrent(NULL))
      .Times(testing::AtLeast(0))
      .WillRepeatedly(testing::Return(false));
  auto result = std::make_unique<
      SharedImageVideoOhosNativeImage::SharedImageRepresentationGLTextureVideo>(
      manager.get(), backing.get(), tracker.get(), std::move(texture));
  result.reset();
}

}  // namespace gpu
