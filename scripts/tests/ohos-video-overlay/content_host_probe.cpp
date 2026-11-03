// Temporary debug-HAP probe for the native ContentSlot host and wire path.
// The generated primary is a fixture, not a Chromium webpage.
#include <IPCKit/ipc_cparcel.h>
#include <arkui/native_node_napi.h>
#include <dlfcn.h>
#include <hilog/log.h>
#include <napi/native_api.h>
#include <native_buffer/native_buffer.h>
#include <unistd.h>
#include <cstring>
#include "ohos_overlay_bridge.h"
#include "ohos_overlay_transport.h"
using namespace ui::ohos_overlay;
namespace {
int endpoint = -1;
int stage = 0;
void Log(const char* message, int value) {
  OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F01, "LumieOverlayHost",
               "%{public}s=%{public}d", message, value);
}
napi_value Number(napi_env env, int n) {
  napi_value result;
  napi_create_int32(env, n, &result);
  return result;
}
}  // namespace
extern "C" napi_value StartOverlayContentProbe(napi_env env,
                                               napi_callback_info info) {
  size_t count = 1;
  napi_value args[1];
  napi_get_cb_info(env, info, &count, args, nullptr, nullptr);
  ArkUI_NodeContentHandle content = nullptr;
  uv_loop_s* loop = nullptr;
  if (count != 1 || endpoint >= 0 ||
      OH_ArkUI_GetNodeContentFromNapiValue(env, args[0], &content) ||
      napi_get_uv_event_loop(env, &loop) != napi_ok)
    return Number(env, -1);
  if (!OhosOverlayAttachContent(OHOS_OVERLAY_BRIDGE_VERSION, "content-probe",
                                content, loop, nullptr, nullptr)) {
    Log("attach_failed", 1);
    return Number(env, -2);
  }
  endpoint = OhosOverlayConnect("content-probe");
  Log("connected", endpoint >= 0);
  if (endpoint < 0)
    return Number(env, -3);
  OH_NativeBuffer_Config config{};
  config.width = 256;
  config.height = 192;
  config.format = NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
  config.usage = NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE |
                 NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_HW_RENDER |
                 NATIVEBUFFER_USAGE_HW_TEXTURE;
  auto* buffer = OH_NativeBuffer_Alloc(&config);
  void* pixels = nullptr;
  if (!buffer || OH_NativeBuffer_Map(buffer, &pixels))
    return Number(env, -4);
  OH_NativeBuffer_GetConfig(buffer, &config);
  for (int y = 0; y < 192; ++y)
    for (int x = 0; x < 256; ++x) {
      auto* pixel = static_cast<uint8_t*>(pixels) + y * config.stride + x * 4;
      pixel[0] = 32;
      pixel[1] = 180;
      pixel[2] = 220;
      pixel[3] = 255;
    }
  OH_NativeBuffer_Unmap(buffer);
  OH_NativeBuffer_SetColorSpace(buffer, OH_COLORSPACE_SRGB_FULL);
  using Writer = int32_t (*)(OH_NativeBuffer*, OHIPCParcel*);
  void* library = dlopen("libnative_buffer.so", RTLD_NOW | RTLD_NOLOAD);
  auto writer = library ? reinterpret_cast<Writer>(
                              dlsym(library, "OH_NativeBuffer_WriteToParcel"))
                        : nullptr;
  if (library)
    dlclose(library);
  auto* parcel = OH_IPCParcel_Create();
  WireMessage frame;
  auto& p = frame.packet;
  p.sequence = 1;
  p.primary.content = 1;
  p.primary.source = p.primary.destination = {0, 0, 256, 192};
  bool valid = parcel && writer && !writer(buffer, parcel);
  if (valid) {
    auto* bytes = OH_IPCParcel_ReadBuffer(parcel, 56);
    valid = bytes;
    if (bytes)
      memcpy(p.prefix.data(), bytes, 56);
  }
  if (valid && p.prefix[2] <= 6) {
    for (unsigned i = 0; i < p.prefix[2] + 1; ++i) {
      int fd = -1;
      if (OH_IPCParcel_ReadFileDescriptor(parcel, &fd) || fd < 0) {
        valid = false;
        break;
      }
      frame.fds.push_back(fd);
    }
    p.fd_count = frame.fds.size();
    int remaining = OH_IPCParcel_GetReadableBytes(parcel);
    if (remaining < 0 || remaining > 1044)
      valid = false;
    else if (remaining) {
      auto* bytes = OH_IPCParcel_ReadBuffer(parcel, remaining);
      valid = bytes;
      if (bytes) {
        memcpy(p.tail.data(), bytes, remaining);
        p.tail_bytes = remaining;
      }
    }
  } else
    valid = false;
  if (parcel)
    OH_IPCParcel_Destroy(parcel);
  OH_NativeBuffer_Unreference(buffer);
  valid = valid && SendWireMessage(endpoint, frame);
  Log("sent", valid);
  stage = valid ? 1 : -5;
  return Number(env, stage);
}
extern "C" napi_value PollOverlayContentProbe(napi_env env,
                                              napi_callback_info) {
  if (endpoint >= 0) {
    WireMessage msg;
    while (ReceiveWireMessage(endpoint, &msg)) {
      Log("reply", static_cast<int>(msg.packet.operation));
      if (msg.packet.operation == WireOp::kProcessed)
        stage = 3;
      if (msg.packet.operation == WireOp::kRejected)
        stage = -10 - static_cast<int>(msg.packet.result);
    }
  }
  return Number(env, stage);
}
extern "C" napi_value StopOverlayContentProbe(napi_env env,
                                              napi_callback_info) {
  if (endpoint >= 0)
    close(endpoint);
  endpoint = -1;
  OhosOverlayDetachContent("content-probe");
  stage = 0;
  Log("stopped", 1);
  return Number(env, 0);
}
