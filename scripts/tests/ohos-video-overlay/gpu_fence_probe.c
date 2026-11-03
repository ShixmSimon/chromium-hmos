// Debug-only real GPU sync-fd producer. No fake eventfd/software fence is used.
// A host-signaled timeline gates a GPU image clear (or an empty capability
// submission) and an exportable binary semaphore. A worker always opens the gate.
#define _POSIX_C_SOURCE 200809L
#define VK_USE_PLATFORM_OHOS
#include <vulkan/vulkan.h>
#include <native_buffer/native_buffer.h>
#include <hilog/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#define LOG(...) OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F00, "LumieGpuFence", __VA_ARGS__)
typedef struct {
    VkInstance instance;
    VkDevice device;
    VkQueue queue;
    VkSemaphore timeline, binary;
    VkImage image;
    VkDeviceMemory memory;
    VkCommandPool pool;
    OH_NativeBuffer* native;
    PFN_vkSignalSemaphoreKHR signal;
    atomic_bool exportFinished;
    uint64_t submittedNs;
    unsigned delayMs;
} FenceJob;

uint64_t GpuFenceNow(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static void Destroy(FenceJob* j)
{
    if (j->device) {
        if (j->pool) vkDestroyCommandPool(j->device, j->pool, NULL);
        if (j->image) vkDestroyImage(j->device, j->image, NULL);
        if (j->memory) vkFreeMemory(j->device, j->memory, NULL);
        if (j->binary) vkDestroySemaphore(j->device, j->binary, NULL);
        if (j->timeline) vkDestroySemaphore(j->device, j->timeline, NULL);
        vkDestroyDevice(j->device, NULL);
    }
    if (j->native) OH_NativeBuffer_Unreference(j->native);
    if (j->instance) vkDestroyInstance(j->instance, NULL);
    free(j);
}

static void* SignalLater(void* opaque)
{
    FenceJob* j = opaque;
    uint64_t deadline = j->submittedNs + (uint64_t)j->delayMs * 1000000;
    for (;;) {
        uint64_t now = GpuFenceNow();
        if (now >= deadline) break;
        uint64_t remain = deadline - now;
        struct timespec pause = { (time_t)(remain / 1000000000), (long)(remain % 1000000000) };
        nanosleep(&pause, NULL);
    }
    VkSemaphoreSignalInfo signal = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = j->timeline, .value = 1 };
    VkResult rc = j->signal(j->device, &signal);
    LOG("gate_signal rc=%{public}d monotonic_ns=%{public}llu elapsed_ms=%{public}llu", rc,
        (unsigned long long)GpuFenceNow(), (unsigned long long)((GpuFenceNow() - j->submittedNs) / 1000000));
    VkResult idle = vkQueueWaitIdle(j->queue);
    LOG("gpu_queue_idle rc=%{public}d monotonic_ns=%{public}llu", idle, (unsigned long long)GpuFenceNow());
    while (!atomic_load_explicit(&j->exportFinished, memory_order_acquire)) {
        struct timespec pause = {0, 1000000}; nanosleep(&pause, NULL);
    }
    Destroy(j);
    return NULL;
}

// Returns an owned, currently unsignaled sync-fd, or -1 for an unsupported or
// inconclusive experiment. Caller must close it or transfer its ownership.
int GpuFenceCreateDelayed(unsigned delayMs, OH_NativeBuffer** imageOut)
{
    if (delayMs < 100 || delayMs > 2000) return -1;
    FenceJob* j = calloc(1, sizeof(*j));
    if (!j) return -1;
    atomic_init(&j->exportFinished, false);
    j->delayMs = delayMs;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "LumieFenceProbe", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkResult rc = vkCreateInstance(&ic, NULL, &j->instance);
    if (rc != VK_SUCCESS) goto fail;
    uint32_t count = 8;
    VkPhysicalDevice physicals[8];
    rc = vkEnumeratePhysicalDevices(j->instance, &count, physicals);
    if (rc != VK_SUCCESS || !count) goto fail;
    VkPhysicalDevice physical = physicals[0];
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    LOG("device=%{public}s api=%{public}u.%{public}u", properties.deviceName,
        VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion));
    uint32_t extensionCount = 0;
    rc = vkEnumerateDeviceExtensionProperties(physical, NULL, &extensionCount, NULL);
    if (rc != VK_SUCCESS || extensionCount > 1024) goto fail;
    VkExtensionProperties* extensions = calloc(extensionCount, sizeof(*extensions));
    if (!extensions) goto fail;
    rc = vkEnumerateDeviceExtensionProperties(physical, NULL, &extensionCount, extensions);
    bool hasTimeline = false, hasFd = false, hasNative = false, hasForeign = false;
    for (uint32_t i = 0; rc == VK_SUCCESS && i < extensionCount; ++i) {
        hasTimeline |= strcmp(extensions[i].extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0;
        hasFd |= strcmp(extensions[i].extensionName, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) == 0;
        hasNative |= strcmp(extensions[i].extensionName, VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME) == 0;
        hasForeign |= strcmp(extensions[i].extensionName, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) == 0;
    }
    free(extensions);
    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeature = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    VkPhysicalDeviceFeatures2 features = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &timelineFeature };
    vkGetPhysicalDeviceFeatures2(physical, &features);
    VkPhysicalDeviceExternalSemaphoreInfo externalInfo = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
    VkExternalSemaphoreProperties external = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
    vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &externalInfo, &external);
    LOG("capability timeline_extension=%{public}d timeline_feature=%{public}u fd_extension=%{public}d export_features=%{public}u",
        hasTimeline, timelineFeature.timelineSemaphore, hasFd, external.externalSemaphoreFeatures);
    if (!hasTimeline || !hasFd || !timelineFeature.timelineSemaphore ||
        !(external.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) goto fail;
    LOG("native_image_capability import=%{public}d foreign_queue=%{public}d", hasNative, hasForeign);
    if (imageOut && (!hasNative || !hasForeign)) goto fail;
    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, NULL);
    if (!queueCount || queueCount > 64) goto fail;
    VkQueueFamilyProperties queues[64];
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, queues);
    uint32_t family = 0;
    while (family < queueCount && !(queues[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    if (family == queueCount) goto fail;
    float priority = 0.1f;
    VkDeviceQueueCreateInfo qc = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
    const char* names[] = {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
    VkDeviceCreateInfo dc = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &timelineFeature, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc,
        .enabledExtensionCount = imageOut ? 4 : 2, .ppEnabledExtensionNames = names };
    rc = vkCreateDevice(physical, &dc, NULL, &j->device);
    if (rc != VK_SUCCESS) goto fail;
    vkGetDeviceQueue(j->device, family, 0, &j->queue);
    j->signal = (PFN_vkSignalSemaphoreKHR)vkGetDeviceProcAddr(j->device, "vkSignalSemaphoreKHR");
    PFN_vkGetSemaphoreFdKHR getFd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(j->device, "vkGetSemaphoreFdKHR");
    if (!j->signal || !getFd) goto fail;
    VkSemaphoreTypeCreateInfo type = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0 };
    VkSemaphoreCreateInfo sc = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type };
    rc = vkCreateSemaphore(j->device, &sc, NULL, &j->timeline);
    if (rc != VK_SUCCESS) goto fail;
    VkExportSemaphoreCreateInfo exportInfo = { .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
    sc.pNext = &exportInfo;
    rc = vkCreateSemaphore(j->device, &sc, NULL, &j->binary);
    if (rc != VK_SUCCESS) goto fail;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (imageOut) {
        // Same OHOS import contract as overlay/ui/ozone/platform/ohos/ohos_video_vulkan.cc.
        OH_NativeBuffer_Config config = {.width=320, .height=180, .format=NATIVEBUFFER_PIXEL_FMT_RGBA_8888,
            .usage=NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_HW_RENDER |
                NATIVEBUFFER_USAGE_HW_TEXTURE | NATIVEBUFFER_USAGE_MEM_DMA};
        j->native = OH_NativeBuffer_Alloc(&config);
        void* pixels = NULL;
        if (!j->native || OH_NativeBuffer_Map(j->native, &pixels) || !pixels) goto fail;
        OH_NativeBuffer_GetConfig(j->native, &config);
        for (int y=0; y<180; ++y) {
            uint32_t* row = (void*)((char*)pixels + y * config.stride);
            for (int x=0; x<320; ++x) row[x] = 0xff0000ff; // Red sentinel: must never be displayed.
        }
        OH_NativeBuffer_Unmap(j->native);
        PFN_vkGetNativeBufferPropertiesOHOS query =
            (PFN_vkGetNativeBufferPropertiesOHOS)vkGetDeviceProcAddr(j->device, "vkGetNativeBufferPropertiesOHOS");
        if (!query) goto fail;
        VkNativeBufferFormatPropertiesOHOS format = {.sType=VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS};
        VkNativeBufferPropertiesOHOS properties = {.sType=VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS, .pNext=&format};
        rc = query(j->device, j->native, &properties);
        LOG("native_image_properties rc=%{public}d format=%{public}u features=%{public}u size=%{public}llu",
            rc, format.format, format.formatFeatures, (unsigned long long)properties.allocationSize);
        if (rc != VK_SUCCESS || !properties.memoryTypeBits || !properties.allocationSize ||
            format.format != VK_FORMAT_R8G8B8A8_UNORM || !(format.formatFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) goto fail;
        VkExternalMemoryImageCreateInfo externalImage = {.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OH_NATIVE_BUFFER_BIT_OHOS};
        VkImageCreateInfo imageInfo = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext=&externalImage,
            .imageType=VK_IMAGE_TYPE_2D, .format=VK_FORMAT_R8G8B8A8_UNORM, .extent={320,180,1},
            .mipLevels=1, .arrayLayers=1, .samples=VK_SAMPLE_COUNT_1_BIT, .tiling=VK_IMAGE_TILING_OPTIMAL,
            .usage=VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode=VK_SHARING_MODE_EXCLUSIVE, .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
        rc = vkCreateImage(j->device, &imageInfo, NULL, &j->image);
        if (rc == VK_ERROR_INVALID_EXTERNAL_HANDLE || rc == VK_ERROR_FORMAT_NOT_SUPPORTED) {
            externalImage.handleTypes=0x00002000;
            rc=vkCreateImage(j->device, &imageInfo, NULL, &j->image);
        }
        if (rc != VK_SUCCESS) goto fail;
        uint32_t memoryType=0;
        while (!(properties.memoryTypeBits & (1u << memoryType))) ++memoryType;
        VkImportNativeBufferInfoOHOS import = {.sType=VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS, .buffer=j->native};
        VkMemoryDedicatedAllocateInfo dedicated = {.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext=&import, .image=j->image};
        VkMemoryAllocateInfo allocation = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext=&dedicated, .allocationSize=properties.allocationSize, .memoryTypeIndex=memoryType};
        rc=vkAllocateMemory(j->device,&allocation,NULL,&j->memory);
        if (rc != VK_SUCCESS) goto fail;
        rc=vkBindImageMemory(j->device,j->image,j->memory,0);
        if (rc != VK_SUCCESS) goto fail;
        VkCommandPoolCreateInfo poolInfo = {.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex=family};
        rc=vkCreateCommandPool(j->device,&poolInfo,NULL,&j->pool);
        if (rc != VK_SUCCESS) goto fail;
        VkCommandBufferAllocateInfo commandInfo = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=j->pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=1};
        rc=vkAllocateCommandBuffers(j->device,&commandInfo,&command);
        if (rc != VK_SUCCESS) goto fail;
        VkCommandBufferBeginInfo begin = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        rc=vkBeginCommandBuffer(command,&begin);
        if (rc != VK_SUCCESS) goto fail;
        VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageMemoryBarrier barrier = {.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT,
            .dstQueueFamilyIndex=family, .image=j->image, .subresourceRange=range};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkClearColorValue color={{221.0f/255,0,221.0f/255,1}};
        vkCmdClearColorImage(command,j->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=0;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex=family; barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,NULL,0,NULL,1,&barrier);
        rc=vkEndCommandBuffer(command);
        if (rc != VK_SUCCESS) goto fail;
        LOG("gpu_image_recorded sequence=%{public}u sentinel=red target=magenta", OH_NativeBuffer_GetSeqNum(j->native));
    }
    uint64_t waitValue = 1, signalValue = 0;
    VkTimelineSemaphoreSubmitInfo ti = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &waitValue,
        .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &signalValue };
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &ti,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &j->timeline, .pWaitDstStageMask = &stage,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &j->binary };
    if (command) {submit.commandBufferCount=1; submit.pCommandBuffers=&command;}
    j->submittedNs = GpuFenceNow();
    rc = vkQueueSubmit(j->queue, 1, &submit, VK_NULL_HANDLE);
    if (rc != VK_SUCCESS) goto fail;
    pthread_t worker;
    if (pthread_create(&worker, NULL, SignalLater, j)) {
        VkSemaphoreSignalInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
            .semaphore = j->timeline, .value = 1 };
        j->signal(j->device, &si); vkQueueWaitIdle(j->queue); goto fail;
    }
    pthread_detach(worker);
    VkSemaphoreGetFdInfoKHR fi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = j->binary, .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT };
    int fd = -1;
    rc = getFd(j->device, &fi, &fd);
    struct pollfd pollFd = {fd, POLLIN, 0};
    int ready = fd >= 0 ? poll(&pollFd, 1, 0) : -1;
    LOG("export rc=%{public}d fd=%{public}d ready=%{public}d revents=%{public}d monotonic_ns=%{public}llu elapsed_ms=%{public}llu",
        rc, fd, ready, pollFd.revents, (unsigned long long)GpuFenceNow(),
        (unsigned long long)((GpuFenceNow() - j->submittedNs) / 1000000));
    if (rc == VK_SUCCESS && fd >= 0 && ready == 0) {
        if (imageOut) {OH_NativeBuffer_Reference(j->native); *imageOut=j->native;}
        atomic_store_explicit(&j->exportFinished, true, memory_order_release);
        return fd;
    }
    atomic_store_explicit(&j->exportFinished, true, memory_order_release);
    if (fd >= 0) close(fd);
    return -1;
fail:
    LOG("fence_probe_unavailable vk_result=%{public}d", rc);
    Destroy(j);
    return -1;
}
