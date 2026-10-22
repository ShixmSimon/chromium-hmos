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
#include "gpu/command_buffer/service/shared_image/skia_vk_hw_video_native_buffer_image_representation.h"
#include "gpu/command_buffer/service/shared_image/skia_vk_ohos_native_buffer_image_representation.h"
#include "gpu/command_buffer/service/shared_image/skia_vk_samelayer_native_buffer_image_representation.h"
#undef private
#undef protected

#include "base/ohos/scoped_native_buffer_handle.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/posix/eintr_wrapper.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_mock_time_task_runner.h"
#include "base/test/task_environment.h"
#include "gpu/command_buffer/service/vulkan_context_provider.h"
#include "gpu/command_buffer/service/ohos/ohos_image_backing.h"
#include "gpu/command_buffer/common/mailbox.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "gpu/command_buffer/service/shared_context_state.h"
#include "gpu/command_buffer/service/shared_image/shared_image_backing.h"
#include "gpu/command_buffer/service/shared_image/test_image_backing.h"
#include "gpu/command_buffer/service/texture_manager.h"
#include "gpu/command_buffer/service/shared_image/shared_image_test_base.h"
#include "gpu/config/gpu_finch_features.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "third_party/dawn/include/dawn/dawn_proc.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSemaphore.h"
#include "third_party/skia/include/gpu/MutableTextureState.h"
#include "third_party/skia/include/gpu/ganesh/GrContextThreadSafeProxy.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_surface.h"
#include "ui/gl/gl_utils.h"
#include "ui/gl/gpu_timing.h"
#include "ui/gl/scoped_make_current.h"
#include "ui/gl/init/gl_factory.h"
#include "ui/gl/test/gl_surface_test_support.h"
#include "ui/gl/test/gl_test_support.h"

using namespace gpu;
using testing::_;
using testing::Return;

namespace gpu {

constexpr int SIZE = 256;

class MockOhosImageBacking : public OhosImageBacking {
public:
    MockOhosImageBacking(const Mailbox& mailbox,
                         viz::SharedImageFormat format,
                         const gfx::Size& size,
                         const gfx::ColorSpace& color_space,
                         GrSurfaceOrigin surface_origin,
                         SkAlphaType alpha_type,
                         SharedImageUsageSet usage,
                         std::string debug_label,
                         size_t estimated_size,
                         bool is_thread_safe,
                         base::ScopedFD initial_upload_fd)
        : OhosImageBacking(mailbox, format, size, color_space,
                           surface_origin, alpha_type, usage, debug_label,
                           estimated_size, is_thread_safe, std::move(initial_upload_fd)) {}

    SharedImageBackingType GetType() const override {
        return SharedImageBackingType::kTest;
    }

    void SetOverlayAccessing(bool accessing) {
        AutoLock auto_lock(this);
        is_overlay_accessing_ = accessing;
    }

    bool IsUsingVulkan() const {
        return features::IsUsingVulkan();
    }

    void SetWriteSyncFd(base::ScopedFD fd) {
        AutoLock auto_lock(this);
        write_sync_fd_ = std::move(fd);
    }

    bool HasWriteSyncFd() const {
        AutoLock auto_lock(this);
        return write_sync_fd_.is_valid();
    }

    void SetReadSyncFd(base::ScopedFD fd) {
        AutoLock auto_lock(this);
        read_sync_fd_ = std::move(fd);
    }

    bool HasReadSyncFd() const {
        AutoLock auto_lock(this);
        return read_sync_fd_.is_valid();
    }
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
    MOCK_METHOD(void, ReleaseCurrent, (gl::GLSurface* surface), (override));
    MOCK_METHOD(bool, IsCurrent, (gl::GLSurface* surface), (override));
    MOCK_METHOD(void*, GetHandle, (), (override));
    MOCK_METHOD(const gfx::ExtensionSet&, GetExtensions, (), (override));
    MOCK_METHOD(bool, MakeCurrentImpl, (gl::GLSurface* surface), (override));
    MOCK_METHOD(void, ResetExtensions, (), (override));
};

class MockGrContext : public GrDirectContext {
public:
    MockGrContext() : GrDirectContext(GrBackendApi::kVulkan, GrContextOptions(), nullptr) {}
    ~MockGrContext() override = default;

    MOCK_METHOD(bool, abandoned, (), ());
};

class MockVulkanContextProvider : public gpu::VulkanContextProvider {
public:
    MockVulkanContextProvider() : gr_context_(new MockGrContext()) {}
    ~MockVulkanContextProvider() = default;

    bool InitializeGrContext(const GrContextOptions& context_options) override { return true; }
    gpu::VulkanImplementation* GetVulkanImplementation() override { return nullptr; }
    gpu::VulkanDeviceQueue* GetDeviceQueue() override { return nullptr; }
    GrDirectContext* GetGrContext() override { 
        return gr_context_.get();
    }
    std::optional<uint32_t> GetSyncCpuMemoryLimit() const override { return std::nullopt; }
    GrVkSecondaryCBDrawContext* GetGrSecondaryCBDrawContext() override { return nullptr; }
    void EnqueueSecondaryCBSemaphores(std::vector<VkSemaphore> semaphores) override {}
    void EnqueueSecondaryCBPostSubmitTask(base::OnceClosure closure) override {}
private:
    std::unique_ptr<MockGrContext> gr_context_;
};

class SkiaVkHWVideoNBImageRepresentationTest : public ::testing::Test {
public:
    SkiaVkHWVideoNBImageRepresentationTest() : task_environment_() {}
    void SetUp() override {
        gl::init::InitializeGLNoExtensionsOneOff(
            true, gl::GpuPreference::kDefault);
        tracker_ = std::make_unique<MemoryTypeTracker>(nullptr);
        mailbox_ = Mailbox::Generate();
        auto format = viz::SinglePlaneFormat::kRGBA_8888;
        gfx::Size size(SIZE, SIZE);
        auto color_space = gfx::ColorSpace::CreateSRGB();
        auto surface_origin = kTopLeft_GrSurfaceOrigin;
        auto alpha_type = kPremul_SkAlphaType;
        std::string debug_label = "test";
        size_t estimated_size = 0;
        bool is_thread_safe = true;
        scoped_refptr<gl::GLShareGroup> share_group = new gl::GLShareGroup();
        gl::GLShareGroup* share_group1 = new gl::GLShareGroup();
        scoped_refptr<MockGLContext> context = new MockGLContext(share_group1);
        scoped_refptr<gl::GLSurface> surf;
        SharedContextState::ContextLostCallback context_lost_callback =
            base::BindOnce([](bool lost, error::ContextLostReason reason) {});
        GrContextType gr_context_type = GrContextType::kVulkan;
        gpu::VulkanContextProvider* vulkan_context_provider = new MockVulkanContextProvider();
        viz::MetalContextProvider* metal_context_provider = nullptr;
        gpu::DawnContextProvider* dawn_context_provider = nullptr;
        base::WeakPtr<gpu::MemoryTracker::Observer> peak_memory_monitor = nullptr;
        context_state_ = new SharedContextState(
            share_group, surf, context, false,
            std::move(context_lost_callback), gr_context_type,
            vulkan_context_provider, metal_context_provider, dawn_context_provider,
            peak_memory_monitor, false);
        SharedImageUsageSet usage = {
            SHARED_IMAGE_USAGE_GLES2_READ,   SHARED_IMAGE_USAGE_GLES2_WRITE,
            SHARED_IMAGE_USAGE_RASTER_READ,  SHARED_IMAGE_USAGE_RASTER_WRITE,
            SHARED_IMAGE_USAGE_WEBGPU_WRITE, SHARED_IMAGE_USAGE_SCANOUT};
        backing_ = std::make_unique<MockOhosImageBacking>(
            mailbox_, format, size, color_space, surface_origin, alpha_type, usage, debug_label,
            estimated_size, is_thread_safe, base::ScopedFD());
        OhosImageBacking* backing = backing_.get();
        video_representation_ = std::make_unique<SkiaVkHWVideoNBImageRepresentation>(
            &manager_, backing, context_state_, tracker_.get());
        ASSERT_NE(video_representation_, nullptr);
    }

    void TearDown() override {
        video_representation_.reset();
        backing_.reset();
        context_state_.reset();
    }

protected:
    gpu::Mailbox mailbox_;
    SharedImageManager manager_;
    scoped_refptr<SharedContextState> context_state_;
    std::unique_ptr<MemoryTypeTracker> tracker_;
    std::unique_ptr<MockOhosImageBacking> backing_;
    std::unique_ptr<SkiaVkHWVideoNBImageRepresentation> video_representation_;
    base::test::TaskEnvironment task_environment_{};
};

TEST_F(SkiaVkHWVideoNBImageRepresentationTest, SkiaVkHWVideoNBImageRepresentationTest001)
{
    std::vector<GrBackendSemaphore> begin_semaphores;
    std::vector<GrBackendSemaphore> end_semaphores;
    std::unique_ptr<skgpu::MutableTextureState> end_state = nullptr;
    auto result = video_representation_->BeginWriteAccess(
        &begin_semaphores, &end_semaphores, &end_state);
    EXPECT_TRUE(result.empty());
    video_representation_->EndWriteAccess();
    result = video_representation_->BeginReadAccess(
        &begin_semaphores, &end_semaphores, &end_state);
    video_representation_->EndReadAccess();
    EXPECT_TRUE(result.empty());
    bool ret = video_representation_->BeginAccess(
        true, &begin_semaphores, &end_semaphores, base::ScopedFD());
    video_representation_->EndAccess(true);
    EXPECT_TRUE(ret);
    ret = video_representation_->BeginAccess(
        false, &begin_semaphores, &end_semaphores, base::ScopedFD());
    video_representation_->EndAccess(false);
    EXPECT_TRUE(ret);
    auto state = video_representation_->GetEndAccessState();
    EXPECT_EQ(state, nullptr);
}

class SkiaVkSamelayerNBImageRepresentationTest : public ::testing::Test {
public:
    SkiaVkSamelayerNBImageRepresentationTest() : task_environment_() {}
    void SetUp() override {
        gl::init::InitializeGLNoExtensionsOneOff(
            true, gl::GpuPreference::kDefault);
        tracker_ = std::make_unique<MemoryTypeTracker>(nullptr);
        mailbox_ = Mailbox::Generate();
        auto format = viz::SinglePlaneFormat::kRGBA_8888;
        gfx::Size size(SIZE, SIZE);
        auto color_space = gfx::ColorSpace::CreateSRGB();
        auto surface_origin = kTopLeft_GrSurfaceOrigin;
        auto alpha_type = kPremul_SkAlphaType;
        std::string debug_label = "test";
        size_t estimated_size = 0;
        bool is_thread_safe = true;
        scoped_refptr<gl::GLShareGroup> share_group = new gl::GLShareGroup();
        gl::GLShareGroup* share_group1 = new gl::GLShareGroup();
        scoped_refptr<MockGLContext> context = new MockGLContext(share_group1);
        scoped_refptr<gl::GLSurface> surf;
        SharedContextState::ContextLostCallback context_lost_callback =
            base::BindOnce([](bool lost, error::ContextLostReason reason) {});
        GrContextType gr_context_type = GrContextType::kVulkan;
        gpu::VulkanContextProvider* vulkan_context_provider = new MockVulkanContextProvider();
        viz::MetalContextProvider* metal_context_provider = nullptr;
        gpu::DawnContextProvider* dawn_context_provider = nullptr;
        base::WeakPtr<gpu::MemoryTracker::Observer> peak_memory_monitor = nullptr;
        context_state_ = new SharedContextState(
            share_group, surf, context, false,
            std::move(context_lost_callback), gr_context_type,
            vulkan_context_provider, metal_context_provider, dawn_context_provider,
            peak_memory_monitor, false);
        SharedImageUsageSet usage = {
            SHARED_IMAGE_USAGE_GLES2_READ,   SHARED_IMAGE_USAGE_GLES2_WRITE,
            SHARED_IMAGE_USAGE_RASTER_READ,  SHARED_IMAGE_USAGE_RASTER_WRITE,
            SHARED_IMAGE_USAGE_WEBGPU_WRITE, SHARED_IMAGE_USAGE_SCANOUT};
        backing_ = std::make_unique<MockOhosImageBacking>(
            mailbox_, format, size, color_space, surface_origin, alpha_type, usage, debug_label,
            estimated_size, is_thread_safe, base::ScopedFD());
        OhosImageBacking* backing = backing_.get();
        samelayer_representation_ = std::make_unique<SkiaVkSamelayerNBImageRepresentation>(
            &manager_, backing, context_state_, tracker_.get());
        ASSERT_NE(samelayer_representation_, nullptr);
    }

    void TearDown() override {
        samelayer_representation_.reset();
        backing_.reset();
        context_state_.reset();
    }

protected:
    gpu::Mailbox mailbox_;
    SharedImageManager manager_;
    scoped_refptr<SharedContextState> context_state_;
    std::unique_ptr<MemoryTypeTracker> tracker_;
    std::unique_ptr<MockOhosImageBacking> backing_;
    std::unique_ptr<SkiaVkSamelayerNBImageRepresentation> samelayer_representation_;
    base::test::TaskEnvironment task_environment_{};
};

TEST_F(SkiaVkSamelayerNBImageRepresentationTest, SkiaVkSamelayerNBImageRepresentationTest001)
{
    std::vector<GrBackendSemaphore> begin_semaphores;
    std::vector<GrBackendSemaphore> end_semaphores;
    std::unique_ptr<skgpu::MutableTextureState> end_state = nullptr;
    auto result = samelayer_representation_->BeginWriteAccess(
        &begin_semaphores, &end_semaphores, &end_state);
    EXPECT_TRUE(result.empty());
    samelayer_representation_->EndWriteAccess();
    result = samelayer_representation_->BeginReadAccess(
        &begin_semaphores, &end_semaphores, &end_state);
    samelayer_representation_->EndReadAccess();
    EXPECT_TRUE(result.empty());
    bool ret = samelayer_representation_->BeginAccess(
        true, &begin_semaphores, &end_semaphores, base::ScopedFD());
    samelayer_representation_->EndAccess(true);
    EXPECT_TRUE(ret);
    ret = samelayer_representation_->BeginAccess(
        false, &begin_semaphores, &end_semaphores, base::ScopedFD());
    samelayer_representation_->EndAccess(false);
    EXPECT_TRUE(ret);
    auto state = samelayer_representation_->GetEndAccessState();
    EXPECT_EQ(state, nullptr);
}

}  // namespace gpu