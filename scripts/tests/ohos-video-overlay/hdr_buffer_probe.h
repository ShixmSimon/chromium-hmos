#ifndef LUMIE_HDR_BUFFER_PROBE_H_
#define LUMIE_HDR_BUFFER_PROBE_H_

#include <stdint.h>
#include <native_buffer/native_buffer.h>

#ifdef __cplusplus
extern "C" {
#endif

// Debug only, API 12+. Borrowed buffer: caller holds its decoder lease and
// serializes access until this synchronous call returns. No map, writes, refs,
// fence consumption or ownership transfer. Sample first frame / format change,
// not every frame. stage: 0 = decoder output, 1 = before RS submission.
// IDs belong to caller's session; pair with timestamp/fence/retirement logs.
void LumieInspectHdrBuffer(OH_NativeBuffer *buffer, uint64_t session,
                          uint64_t frame, int stage);

#ifdef __cplusplus
}
#endif
#endif
