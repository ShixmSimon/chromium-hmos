#include "hdr_buffer_probe.h"

#include <cstring>
#include <hilog/log.h>
#include <memory>

namespace {
constexpr unsigned int kDomain = 0xD004301;
constexpr const char* kTag = "LumieHdrBuffer";

void Metadata(OH_NativeBuffer* buffer, uint64_t session, uint64_t frame,
              int stage, OH_NativeBuffer_MetadataKey key) {
  int32_t size = 0;
  uint8_t* bytes = nullptr;
  const int result = OH_NativeBuffer_GetMetadataValue(buffer, key, &size, &bytes);
  // Public graphic_graphic_surface implementation returns new uint8_t[].
  // Do not use free(), or delete the borrowed NativeBuffer. See README sources.
  std::unique_ptr<uint8_t[]> owner(bytes);
  OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
      "session=%{public}llu frame=%{public}llu stage=%{public}d key=%{public}d result=%{public}d size=%{public}d",
      static_cast<unsigned long long>(session), static_cast<unsigned long long>(frame),
      stage, static_cast<int>(key), result, size);
  if (result != 0 || !bytes || size <= 0)
    return;  // Failure is unknown, never inferred as SDR or absent HDR.
  if (key == OH_HDR_METADATA_TYPE) {
    if (size == static_cast<int32_t>(sizeof(OH_NativeBuffer_MetadataType))) {
      OH_NativeBuffer_MetadataType type;
      std::memcpy(&type, bytes, sizeof(type));
      OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "hdr_type=%{public}d", static_cast<int>(type));
    } else {
      OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag, "hdr_type_unrecognized_size=%{public}d", size);
    }
  } else if (key == OH_HDR_STATIC_METADATA &&
             size == static_cast<int32_t>(sizeof(OH_NativeBuffer_StaticMetadata))) {
    OH_NativeBuffer_StaticMetadata metadata;
    std::memcpy(&metadata, bytes, sizeof(metadata));
    const auto& m = metadata.smpte2086;
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
        "mastering R=%{public}g,%{public}g G=%{public}g,%{public}g B=%{public}g,%{public}g W=%{public}g,%{public}g min=%{public}g max=%{public}g CLL=%{public}g FALL=%{public}g",
        m.displayPrimaryRed.x, m.displayPrimaryRed.y, m.displayPrimaryGreen.x,
        m.displayPrimaryGreen.y, m.displayPrimaryBlue.x, m.displayPrimaryBlue.y,
        m.whitePoint.x, m.whitePoint.y, m.minLuminance, m.maxLuminance,
        metadata.cta861.maxContentLightLevel, metadata.cta861.maxFrameAverageLightLevel);
  }
  // Bound diagnostic output, including unknown static layouts. No dynamic SEI
  // extraction here: these fixtures contain no HDR10+/Vivid dynamic metadata.
  char hex[129] = {};
  constexpr char digits[] = "0123456789abcdef";
  const int count = size < 64 ? size : 64;
  for (int i = 0; i < count; ++i) {
    hex[i * 2] = digits[bytes[i] >> 4];
    hex[i * 2 + 1] = digits[bytes[i] & 15];
  }
  OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "key=%{public}d bytes_prefix=%{public}s truncated=%{public}d",
               static_cast<int>(key), hex, size > count);
}
}  // namespace

extern "C" void LumieInspectHdrBuffer(OH_NativeBuffer* buffer, uint64_t session,
                                      uint64_t frame, int stage) {
  if (!buffer) {
    OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, "null_buffer");
    return;
  }
  OH_NativeBuffer_Config config = {};
  OH_NativeBuffer_GetConfig(buffer, &config);
  OH_NativeBuffer_ColorSpace space = OH_COLORSPACE_NONE;
  const int result = OH_NativeBuffer_GetColorSpace(buffer, &space);
  OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
      "session=%{public}llu frame=%{public}llu stage=%{public}d seq=%{public}u width=%{public}d height=%{public}d stride=%{public}d format=%{public}d color_result=%{public}d color=%{public}d",
      static_cast<unsigned long long>(session), static_cast<unsigned long long>(frame), stage,
      OH_NativeBuffer_GetSeqNum(buffer), config.width, config.height, config.stride,
      config.format, result, static_cast<int>(space));
  Metadata(buffer, session, frame, stage, OH_HDR_METADATA_TYPE);
  Metadata(buffer, session, frame, stage, OH_HDR_STATIC_METADATA);
}
