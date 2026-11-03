// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// Debug-only availability probe. Resolves names but NEVER calls private APIs,
// dereferences returned addresses, opens library files, or changes linker policy.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <hilog/log.h>
#include <link.h>
#include <napi/native_api.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define MAX_LIBRARIES 32
#define PATH_BYTES 512
static const char* const kSymbols[] = {
    "_ZN4OHOS5Rosen13RSSurfaceNode15SetDelegateModeEb",
    "_ZN4OHOS5Rosen13RSSurfaceNode18SetDelegateDstRectEffff",
    "_ZN4OHOS5Rosen13RSSurfaceNode18SetDelegateSrcRectEffff",
    "_ZN4OHOS5Rosen13RSSurfaceNode11CleanBufferEb",
    "_ZN4OHOS5Rosen13RSTransaction5BeginEv",
    "_ZN4OHOS5Rosen13RSTransaction6CommitEv",
    "_ZN4OHOS5Rosen13RSTransaction24FlushImplicitTransactionEv",
};
static const char* const kNames[] = {
    "SetDelegateMode", "SetDelegateDstRect", "SetDelegateSrcRect", "CleanBuffer",
    "TransactionBegin", "TransactionCommit", "FlushImplicitTransaction",
};
typedef struct {
    char paths[MAX_LIBRARIES][PATH_BYTES];
    size_t count;
    bool truncated;
} LibraryList;

static int Collect(struct dl_phdr_info* info, size_t size, void* opaque)
{
    (void)size;
    LibraryList* list = opaque;
    const char* path = info->dlpi_name;
    if (!path) return 0;
    const char* name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (!strstr(name, "render_service") && !strstr(name, "rosen") &&
        strcmp(name, "libace.z.so") != 0) return 0;
    if (list->count >= MAX_LIBRARIES || strlen(path) >= PATH_BYTES) {
        list->truncated = true;
        return 0;
    }
    snprintf(list->paths[list->count++], PATH_BYTES, "%s", path);
    return 0;
}

static unsigned Inspect(void* handle, const char* scope)
{
    unsigned found = 0;
    for (size_t i = 0; i < sizeof(kSymbols) / sizeof(kSymbols[0]); ++i) {
        dlerror();
        void* symbol = dlsym(handle, kSymbols[i]);
        const char* error = dlerror();
        const bool available = symbol != NULL && error == NULL;
        found += available ? 1 : 0;
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F00, "LumieOverlaySymbols",
            "scope=%{public}s symbol=%{public}s available=%{public}d",
            scope, kNames[i], available);
    }
    return found;
}

napi_value ProbeOverlaySymbols(napi_env env, napi_callback_info info)
{
    (void)info;
    LibraryList list = {0};
    // Collect first; never re-enter dlopen while dl_iterate_phdr holds its lock.
    dl_iterate_phdr(Collect, &list);
    unsigned found = Inspect(RTLD_DEFAULT, "default");
    for (size_t i = 0; i < list.count; ++i) {
        const char* path = list.paths[i];
        OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F00, "LumieOverlaySymbols",
            "loaded=%{public}s", path);
        // Only query an already loaded library through the normal app linker.
        // Failure is evidence, not a reason to try another namespace/path.
        void* handle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
        if (!handle) {
            const char* error = dlerror();
            OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F00, "LumieOverlaySymbols",
                "noload_failed=%{public}s reason=%{public}s", path,
                error ? error : "unknown");
            continue;
        }
        found += Inspect(handle, path);
        dlclose(handle);
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F00, "LumieOverlaySymbols",
        "libraries=%{public}zu truncated=%{public}d symbol_hits=%{public}u private_calls=0",
        list.count, list.truncated, found);
    napi_value result;
    napi_create_uint32(env, found, &result);
    return result;
}
