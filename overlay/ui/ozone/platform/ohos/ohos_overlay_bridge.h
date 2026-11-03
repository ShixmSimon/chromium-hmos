// Copyright 2026 The Chromium Authors. All rights reserved.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_BRIDGE_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_OVERLAY_BRIDGE_H_
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define OHOS_OVERLAY_EXPORT __attribute__((visibility("default")))
#define OHOS_OVERLAY_BRIDGE_VERSION 1u
// Producer attestation: this allocation belongs to a fresh SDR-only allocation
// or decoder Surface epoch that has NEVER carried HDR metadata. NOT inferred
// from a getter error. Must be cleared when an allocation/queue ever carries
// HDR, and cannot be restored until that queue/allocation is replaced/drained.
#define OHOS_OVERLAY_SDR_TAGS_PRISTINE 1u
#define OHOS_OVERLAY_REUSE_CONTENT 2u
typedef struct OhosOverlayHost OhosOverlayHost;
typedef struct {
  float x, y, width, height;
} OhosOverlayRect;
typedef struct {
  void* native_buffer;
  // Borrowed sync_file FD; -1 means producer has finished. Duplicated on entry.
  int acquire_fence;
  uint32_t flags;
  OhosOverlayRect source;
  OhosOverlayRect destination;
  // Actual decoder ConsumerSurface/GPU read lease. The callback is consumed
  // ONLY on ACCEPTED and invoked exactly once after both fences and server
  // release, on the host's owning thread. It must dispatch to the decoder's
  // sequence as necessary. It MUST NOT call back into this host synchronously.
  void* lease_context;
  void (*release_lease)(void*);
} OhosOverlayPlane;
typedef struct {
  uint32_t version;
  uint32_t has_video;
  uint32_t primary_has_video_hole;
  uint32_t reserved;
  OhosOverlayPlane primary;
  OhosOverlayPlane video;
} OhosOverlayFrame;
typedef enum {
  OHOS_OVERLAY_ACCEPTED = 0,
  OHOS_OVERLAY_UNSUPPORTED = 1,
  OHOS_OVERLAY_BUSY = 2,
  OHOS_OVERLAY_PREPARE_FAILED = 3,
  OHOS_OVERLAY_CLOSED = 4
} OhosOverlayResult;
typedef struct {
  uint32_t version;
  uint32_t state;  // 0 running, 1 waiting for commands, 2 draining, 3 closed.
  uint32_t outstanding_allocations;
  uint32_t commands_processed;  // NOT physical presentation feedback.
  uint64_t last_submission;
} OhosOverlayStatus;

// Browser UI integration. node_content is an ArkUI_NodeContentHandle and loop
// is the owning napi uv_loop_t. The content slot must cover the web viewport,
// clip children and ignore hit testing. Attach/Detach run on that UI thread.
OHOS_OVERLAY_EXPORT int OhosOverlayAttachContent(uint32_t version,
                                                 const char* component_id,
                                                 void* node_content,
                                                 void* loop,
                                                 void* owner,
                                                 void (*release_owner)(void*));
OHOS_OVERLAY_EXPORT void OhosOverlayDetachContent(const char* component_id);
// Broker worker only: returns an owned nonblocking SOCK_SEQPACKET endpoint.
// -1 means unavailable; never wait for UI under the browser presentation lock.
OHOS_OVERLAY_EXPORT int OhosOverlayConnect(const char* component_id);
// Browser decoder registration. On success consumes the read lease callback;
// Unregister removes lookup eligibility, but active display reads keep it
// alive.
OHOS_OVERLAY_EXPORT int OhosOverlayRegisterVideo(uint64_t token,
                                                 void* native_buffer,
                                                 uint32_t flags,
                                                 void* lease_context,
                                                 void (*release_lease)(void*));
OHOS_OVERLAY_EXPORT void OhosOverlayUnregisterVideo(uint64_t token);

// Same-process pointers only. GPU-process buffers must be parcel-imported by
// the browser before entry; never send any of these pointers over IPC.
// NULL means unsupported context/firmware or listener already owned elsewhere.
OHOS_OVERLAY_EXPORT OhosOverlayHost* OhosOverlayCreate(uint32_t version,
                                                       uint64_t context_token,
                                                       uint64_t parent_node_id);
OHOS_OVERLAY_EXPORT OhosOverlayResult
OhosOverlaySubmit(OhosOverlayHost*, const OhosOverlayFrame*);
OHOS_OVERLAY_EXPORT void OhosOverlayPoll(OhosOverlayHost*, OhosOverlayStatus*);
OHOS_OVERLAY_EXPORT void OhosOverlayClose(OhosOverlayHost*);
// Returns 0 while closing: keep host and poll; never destroy by timeout.
OHOS_OVERLAY_EXPORT int OhosOverlayDestroy(OhosOverlayHost*);

#ifdef __cplusplus
}
#endif
#endif
