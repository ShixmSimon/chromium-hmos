// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_MOJO_SERVICES_OHOS_BROWSER_MEDIA_CLIENT_H_
#define MEDIA_MOJO_SERVICES_OHOS_BROWSER_MEDIA_CLIENT_H_

#include <memory>

#include "media/mojo/services/media_mojo_export.h"
#include "media/mojo/services/mojo_media_client.h"

namespace media {

// For the phone fork launcher only. The browser owns AVCodecKit/DRM objects;
// the existing Mojo media protocols carry requests, frames and audio buffers.
// Initialize and use the client on the MediaService sequence, never the UI.
MEDIA_MOJO_EXPORT std::unique_ptr<MojoMediaClient>
CreateOhosBrowserMediaClient();

}  // namespace media

#endif  // MEDIA_MOJO_SERVICES_OHOS_BROWSER_MEDIA_CLIENT_H_
