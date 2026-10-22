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

#include "base/containers/flat_set.h"
#define private public
#define protected public
#include "gpu/command_buffer/service/shared_image/ohos_native_buffer_image_backing.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#undef private
#undef protected

#include "arkweb/ohos_adapter_ndk/ohos_native_buffer_adapter/ohos_native_buffer_adapter_impl.h"
#include "base/ohos/scoped_native_buffer_handle.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/posix/eintr_wrapper.h"
#include "base/synchronization/lock.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_mock_time_task_runner.h"
#include "base/test/task_environment.h"
#include "gpu/command_buffer/service/vulkan_context_provider.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/ohos/native_image_texture_owner.h"
#include "gpu/command_buffer/service/stream_texture_shared_image_interface.h"
#include "gpu/command_buffer/service/abstract_texture_ohos.h"
#include "gpu/command_buffer/service/ohos/native_buffer_config_impl.h"
#include "gpu/command_buffer/service/ohos/ohos_image_backing.h"
#include "gpu/command_buffer/service/ohos/ohos_video_image_backing.h"
#include "gpu/command_buffer/service/ohos/scoped_native_buffer_fence_sync.h"
#include "gpu/command_buffer/service/shared_image/shared_image_backing.h"
#include "gpu/command_buffer/service/shared_image/test_image_backing.h"
#include "gpu/command_buffer/service/texture_manager.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/dawn/include/dawn/dawn_proc.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSemaphore.h"
#include "third_party/skia/include/gpu/MutableTextureState.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_surface.h"
#include "ui/gl/gl_utils.h"
#include "ui/gl/gpu_timing.h"
#include "ui/gl/init/gl_factory.h"
#include "ui/gl/test/gl_surface_test_support.h"
#include "ui/gl/test/gl_test_support.h"
#include "gpu/config/gpu_finch_features.h"

using namespace gpu;
using testing::_;
using testing::Return;
using testing::NiceMock;

namespace gpu {
constexpr int SIZE = 256;
ScopedNativeBufferHandle CreateHandle()
{
    void* buffer = nullptr;
    OH_NativeBuffer_Config config = {
        .width = 10,
        .height = 10,
        .format = OH_NativeBuffer_Format::NATIVEBUFFER_PIXEL_FMT_RGBA_8888,
        .usage = 1,
        .stride = 1,
    };
    buffer = OH_NativeBuffer_Alloc(&config);
    EXPECT_NE(buffer, nullptr);
    auto handle = gpu::ScopedNativeBufferHandle::Adopt(buffer);
    return handle;
}

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
    ~MockSharedContextState();
    MOCK_METHOD(bool, GrContextIsVulkan, (), (const));
    MOCK_METHOD(bool, GrContextIsGL, (), (const));
    MOCK_METHOD(gpu::VulkanContextProvider*, vulkan_context_provider, (), (const));
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

class OhosNativeBufferImageBackingTest : public ::testing::Test {
public:
    OhosNativeBufferImageBackingTest() : task_environment_() {}
    void SetUp() override {
        gl::init::InitializeGLNoExtensionsOneOff(
            /*init_bindings=*/true, /*gpu_preference=*/gl::GpuPreference::kDefault);
        auto display = gl::GLTestSupport::InitializeGL(std::nullopt);
        ASSERT_NE(display, nullptr);
        surf_ = gl::init::CreateOffscreenGLSurface(display, gfx::Size());
        ASSERT_NE(surf_, nullptr);
        gl_context_ = gl::init::CreateGLContext(nullptr, surf_.get(), gl::GLContextAttribs());
        ASSERT_NE(gl_context_, nullptr);
        bool make_current_result = gl_context_->MakeCurrent(surf_.get());
        ASSERT_TRUE(make_current_result);
        tracker_ = std::make_unique<MemoryTypeTracker>(nullptr);
        mailbox_ = Mailbox::Generate();
        format_ = viz::SinglePlaneFormat::kRGBA_8888;
        usage_ = {
            SHARED_IMAGE_USAGE_GLES2_READ,   SHARED_IMAGE_USAGE_GLES2_WRITE,
            SHARED_IMAGE_USAGE_RASTER_READ,  SHARED_IMAGE_USAGE_RASTER_WRITE,
            SHARED_IMAGE_USAGE_WEBGPU_WRITE, SHARED_IMAGE_USAGE_SCANOUT};
        is_thread_safe_ = true;
        size_ = gfx::Size(SIZE, SIZE);
        color_space_ = gfx::ColorSpace::CreateSRGB();
        surface_origin_ = kTopLeft_GrSurfaceOrigin;
        alpha_type_ = kPremul_SkAlphaType;
        debug_label_ = "test";
        backing_ = std::make_unique<OhosNativeBufferImageBacking>(mailbox_,
            format_, size_, color_space_, surface_origin_, alpha_type_, usage_, 
            debug_label_, estimated_size_, is_thread_safe_, base::ScopedFD(), gl_format_caps_);
    }

    void TearDown() override {
        if (gl_context_ && surf_) {
            gl_context_->ReleaseCurrent(surf_.get());
        }
        backing_.reset();
    }

protected:
    gpu::Mailbox mailbox_;
    viz::SharedImageFormat format_;
    gfx::Size size_;
    gfx::ColorSpace color_space_;
    GrSurfaceOrigin surface_origin_;
    SkAlphaType alpha_type_;
    std::string debug_label_;
    std::unique_ptr<MemoryTypeTracker> tracker_;
    SharedImageUsageSet usage_;
    size_t estimated_size_ = 0;
    bool is_thread_safe_;
    GLFormatCaps gl_format_caps_;
    std::unique_ptr<OhosNativeBufferImageBacking> backing_;
    base::test::TaskEnvironment task_environment_{};
    scoped_refptr<gl::GLSurface> surf_;
    scoped_refptr<gl::GLContext> gl_context_;
    SharedImageManager manager_;
};

TEST_F(OhosNativeBufferImageBackingTest, ConstructorAndDestructor)
{
    EXPECT_NE(backing_, nullptr);
    EXPECT_EQ(backing_->mailbox(), mailbox_);
    EXPECT_EQ(backing_->size(), size_);
    EXPECT_EQ(backing_->color_space(), color_space_);
    EXPECT_EQ(backing_->surface_origin(), surface_origin_);
    EXPECT_EQ(backing_->alpha_type(), alpha_type_);
    backing_.reset();
}

TEST_F(OhosNativeBufferImageBackingTest, ProduceGLTexture)
{
    auto representation = backing_->ProduceGLTexture(&manager_, tracker_.get());
    EXPECT_EQ(representation, nullptr);
    auto handle = CreateHandle();
    auto backing = std::make_unique<OhosNativeBufferImageBacking>(
        mailbox_, format_, size_, color_space_, surface_origin_, alpha_type_, usage_, debug_label_,
        std::move(handle), estimated_size_, is_thread_safe_, base::ScopedFD(), false, gl_format_caps_);
    representation = backing->ProduceGLTexture(&manager_, tracker_.get());
    EXPECT_NE(representation, nullptr);
}

TEST_F(OhosNativeBufferImageBackingTest, ProduceGLTexturePassthrough)
{
    auto representation = backing_->ProduceGLTexturePassthrough(&manager_, tracker_.get());
    EXPECT_EQ(representation, nullptr);
    auto handle = CreateHandle();
    auto backing = std::make_unique<OhosNativeBufferImageBacking>(
        mailbox_, format_, size_, color_space_, surface_origin_, alpha_type_, usage_, debug_label_,
        std::move(handle), estimated_size_, is_thread_safe_, base::ScopedFD(), false, gl_format_caps_);
    representation = backing->ProduceGLTexturePassthrough(&manager_, tracker_.get());
    EXPECT_NE(representation, nullptr);
}
}  // namespace gpu