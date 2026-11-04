// Copyright 2026 The Chromium Authors
// Exact firmware ABI profiles. Never combine fingerprints from different
// builds.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_RS_ABI_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_RS_ABI_H_
#include <array>
#include <cstddef>
#include <cstdint>
namespace ui::ohos_overlay {
enum class RsCheckedFunction : size_t {
  kSurfaceConstructor,
  kSurfaceFactory,
  kSurfaceDestructor,
  kContextConstructor,
  kBufferCallback,
  kContextSurfaceFactory,
  kRemoveChild,
  kCleanBuffer,
  kListenerConstructor,
  kListenerRegister,
  kGetSurface,
  kGoBackground,
  kCount,
};
struct RsFingerprint {
  size_t bytes;
  uint32_t hash;
};
struct RsAbiProfile {
  const char* name;
  uint32_t render_node_create_hash;
  std::array<RsFingerprint, static_cast<size_t>(RsCheckedFunction::kCount)>
      functions;
};
// Both builds use the same inspected boundary layouts: RSUIContext node map
// +40, transaction handler +120, remote +480; listener storage 112 bytes;
// RenderNodeStruct shared_ptr +0 and node id +16. RSSurfaceNode grew by 32
// bytes in the second build; only its own factory/accessors access that object.
inline constexpr RsAbiProfile kRsAbiProfiles[] = {
    {"retail-20261103",
     0x2d4d8cabu,
     {{
         {436, 0xe95e87f1u},   // kSurfaceConstructor
         {2168, 0xea6a36b8u},  // kSurfaceFactory
         {1576, 0x7194ed42u},  // kSurfaceDestructor
         {1708, 0x14d660fdu},  // kContextConstructor
         {1588, 0x5290b89eu},  // kBufferCallback
         {232, 0x801e5a23u},   // kContextSurfaceFactory
         {1176, 0xd791aac8u},  // kRemoveChild
         {20, 0x0cd5d9fdu},    // kCleanBuffer
         {228, 0xeeb6d8a9u},   // kListenerConstructor
         {1452, 0xd110289eu},  // kListenerRegister
         {264, 0x108d9ce0u},   // kGetSurface
         {92, 0x2ef7d186u},    // kGoBackground
     }}},
    {"retail-20261104",
     0x18270173u,
     {{
         {428, 0x619939d5u},   // kSurfaceConstructor
         {2168, 0x6b79c3d1u},  // kSurfaceFactory
         {1576, 0xfe0ed564u},  // kSurfaceDestructor
         {1708, 0x0b6a1667u},  // kContextConstructor
         {1588, 0x9015f26fu},  // kBufferCallback
         {232, 0x61a953dfu},   // kContextSurfaceFactory
         {1468, 0x6889bbd2u},  // kRemoveChild
         {20, 0x8964ac72u},    // kCleanBuffer
         {228, 0xee1d0bdcu},   // kListenerConstructor
         {1452, 0xdd2178cau},  // kListenerRegister
         {264, 0x7659cdadu},   // kGetSurface
         {92, 0xccea5842u},    // kGoBackground
     }}},
};
inline const RsAbiProfile* SelectRsAbiProfile(
    uint32_t render_node_create_hash) {
  for (const auto& profile : kRsAbiProfiles) {
    if (profile.render_node_create_hash == render_node_create_hash)
      return &profile;
  }
  return nullptr;
}
inline const RsFingerprint& RsFunctionFingerprint(const RsAbiProfile& profile,
                                                  RsCheckedFunction function) {
  return profile.functions[static_cast<size_t>(function)];
}
}  // namespace ui::ohos_overlay
#endif
