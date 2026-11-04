// Copyright 2026 The Chromium Authors. All rights reserved.
#ifndef UI_OZONE_PLATFORM_OHOS_OHOS_RS_OVERLAY_BACKEND_H_
#define UI_OZONE_PLATFORM_OHOS_OHOS_RS_OVERLAY_BACKEND_H_

#include "ohos_overlay_session.h"

namespace ui::ohos_overlay {
// This translation unit and its caller/session must be built with the OHOS NDK
// legacy __h libc++ callable ABI, outside Chromium's __Cr C++ ABI. Only the C
// interface in ohos_overlay_bridge.h is allowed to cross that boundary.
// Caller supplies its own RS UI context token and parent node ID, never a
// guessed context or another process's node.
// Creation/submission/poll/destruction run on that context's owning UI
// transaction thread. Completion uses the normal transaction callback keyed
// by context/timestamp; never register a delegate compositor sender identity
// for ArkUI's thread. Buffer retirement remains independently fence-protected.
std::unique_ptr<Backend> CreateRsOverlayBackend(uint64_t context_token,
                                                uint64_t parent_node_id);
// Only accepts an ArkUI RenderNode created/owned by this bridge. Returns zero
// on an unrecognised ABI or if the node is not in the current UI context.
uint64_t ResolveOwnedRenderNode(uint64_t context_token, void* render_node);
}  // namespace ui::ohos_overlay
#endif
