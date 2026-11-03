// Debug-only RenderService ABI inventory, restricted to this process's loaded
// modules. Used before constructing test layers; not production capability code.
#define _GNU_SOURCE
#include <elf.h>
#include <link.h>
#include <napi/native_api.h>
#include <hilog/log.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <native_window/external_window.h>
#include <native_buffer/native_buffer.h>

#define LOG(...) OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F00, "LumieRsProbe", __VA_ARGS__)

static bool Contains(const struct dl_phdr_info* info, uintptr_t ptr, size_t bytes)
{
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)* h = &info->dlpi_phdr[i];
        uintptr_t begin = info->dlpi_addr + h->p_vaddr;
        if (h->p_type == PT_LOAD && (h->p_flags & PF_R) && ptr >= begin &&
            ptr - begin <= h->p_memsz && bytes <= h->p_memsz - (ptr - begin)) return true;
    }
    return false;
}

static uintptr_t Pointer(const struct dl_phdr_info* info, uintptr_t value)
{
    return Contains(info, value, 1) ? value : info->dlpi_addr + value;
}

static bool Wanted(const char* s)
{
    if (strstr(s, "__shared_weak_count") || strstr(s, "GetPresentTimestamp") || strstr(s, "GetPresentFence") || strstr(s, "CleanBuffer") || strstr(s, "GoBackground") || strstr(s, "RSSurfaceNodeD")) return true;
    if (strstr(s, "RSUIContext")) return true;
    if (strstr(s, "RSSurfaceNode") && (strstr(s, "Create") || strstr(s, "SetBuffer") ||
        strstr(s, "Delegate") || strstr(s, "GetSurface") || strstr(s, "GetName") ||
        strstr(s, "ConfigC") || strstr(s, "ConfigD") || strstr(s, "NodeC"))) return true;
    if (strstr(s, "RSNodeMap") && (strstr(s, "Instance") || strstr(s, "GetNodeI"))) return true;
    if (strstr(s, "RSUIContext") && (strstr(s, "GetCurrent") || strstr(s, "GetTransaction") ||
        strstr(s, "GetMutableNodeMap") || strstr(s, "GetRSTransaction") || strstr(s, "GetConnect"))) return true;
    if (strstr(s, "RSNode") && (strstr(s, "GetRSUIContext") || strstr(s, "SetBounds") ||
        strstr(s, "SetVisible") || strstr(s, "GetIdE") || strstr(s, "AddChild") || strstr(s, "RemoveChild"))) return true;
    if (strstr(s, "RSBaseNode") && (strstr(s, "AddChild") || strstr(s, "RemoveChild"))) return true;
    if (strstr(s, "SurfaceTransactionListener") || strstr(s, "SurfaceNodeBufferReleaseListener")) return true;
    if (strstr(s, "RSTransactionHandler") && strstr(s, "AddCommand")) return true;
    if (strstr(s, "SurfaceBuffer") && strstr(s, "NativeBuffer")) return true;
    return false;
}

static int Inspect(struct dl_phdr_info* info, size_t size, void* user)
{
    (void)size;
    unsigned* total = user;
    const char* name = info->dlpi_name;
    if (!name || (!strstr(name, "librender_service_client.z.so") &&
        !strstr(name, "libsurface.z.so"))) return 0;
    uintptr_t sym = 0, str = 0, hash = 0, gnu = 0;
    size_t strings = 0;
    for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)* h = &info->dlpi_phdr[i];
        if (h->p_type != PT_DYNAMIC) continue;
        const ElfW(Dyn)* d = (const void*)(info->dlpi_addr + h->p_vaddr);
        for (size_t j = 0; j < h->p_memsz / sizeof(*d) && d[j].d_tag != DT_NULL; ++j) {
            uintptr_t p = Pointer(info, d[j].d_un.d_ptr);
            if (d[j].d_tag == DT_SYMTAB) sym = p;
            if (d[j].d_tag == DT_STRTAB) str = p;
            if (d[j].d_tag == DT_STRSZ) strings = d[j].d_un.d_val;
            if (d[j].d_tag == DT_HASH) hash = p;
            if (d[j].d_tag == DT_GNU_HASH) gnu = p;
        }
    }
    size_t count = 0;
    if (hash && Contains(info, hash, 8)) count = ((uint32_t*)hash)[1];
    if (!count && gnu && Contains(info, gnu, 16)) {
        uint32_t* h = (void*)gnu;
        uintptr_t buckets = gnu + 16 + (size_t)h[2] * sizeof(ElfW(Addr));
        if (h[0] > 1000000 || !Contains(info, buckets, (size_t)h[0] * 4)) return 0;
        uint32_t max = h[1];
        for (unsigned i = 0; i < h[0]; ++i) if (((uint32_t*)buckets)[i] > max) max = ((uint32_t*)buckets)[i];
        uintptr_t chains = buckets + (size_t)h[0] * 4;
        for (size_t i = max; i < 1000000; ++i) {
            uintptr_t c = chains + (i - h[1]) * 4;
            if (!Contains(info, c, 4)) break;
            if (*(uint32_t*)c & 1) { count = i + 1; break; }
        }
    }
    if (!count || count > 1000000 || !Contains(info, sym, count * sizeof(ElfW(Sym))) ||
        !Contains(info, str, strings)) { LOG("inventory_invalid library=%{public}s", name); return 0; }
    const ElfW(Sym)* table = (const void*)sym;
    LOG("library=%{public}s symbols=%{public}zu", name, count);
    for (size_t i = 0; i < count; ++i) {
        const ElfW(Sym)* s = &table[i];
        if (s->st_shndx == SHN_UNDEF || s->st_name >= strings || ELF64_ST_TYPE(s->st_info) != STT_FUNC) continue;
        const char* text = (const void*)(str + s->st_name);
        if (!memchr(text, 0, strings - s->st_name) || !Wanted(text)) continue;
        LOG("export=%{public}s bytes=%{public}zu", text, (size_t)s->st_size);
        if ((strstr(text, "RSSurfaceNode") && (strstr(text, "6CreateE") || strstr(text, "NodeC2") ||
             strstr(text, "SetBufferAvailable"))) || strstr(text, "RSUIContextC2") ||
             strstr(text, "SurfaceTransactionListenerC2") || strstr(text, "CleanBuffer") ||
             strstr(text, "RSSurfaceNodeD2") || strstr(text, "__release_shared") ||
             strstr(text, "RSSurfaceNode10GetSurface") || strstr(text, "ProducerSurface12GoBackground") ||
             (strstr(text, "RSNode11RemoveChild")) ||
             (strstr(text, "SurfaceTransactionListener") && strstr(text, "RegisterCommand"))) {
            size_t bytes = s->st_size < 4096 ? s->st_size : 4096;
            uintptr_t code = info->dlpi_addr + s->st_value;
            if (Contains(info, code, bytes)) {
                for (size_t off = 0; off < bytes; off += 32) {
                    char hex[65] = {0};
                    size_t n = bytes - off < 32 ? bytes - off : 32;
                    for (size_t j = 0; j < n; ++j) snprintf(hex + 2*j, 3, "%02x", ((unsigned char*)code)[off+j]);
                    LOG("code=%{public}s offset=%{public}zu pc=%{public}zu hex=%{public}s", text, off,
                        (size_t)s->st_value + off, hex);
                }
            }
        }
        ++*total;
    }
    return 0;
}

napi_value ProbeRsApi(napi_env env, napi_callback_info info)
{
    (void)info;
    unsigned total = 0;
    dl_iterate_phdr(Inspect, &total);
    LOG("inventory_done exports=%{public}u", total);
    napi_value result;
    napi_create_uint32(env, total, &result);
    return result;
}

// AAPCS64 indirect return for non-trivial system shared_ptr/sptr objects.
// Default application libc++ objects do not cross the system ABI boundary.
// Callbacks are compiled separately using the pinned legacy __h ABI.
// Fixed test references are retained until the debug process is stopped.
typedef struct { void* object; void* control; } SystemShared;
static SystemShared context, nodes[2], root, appCanvas;
static void* library;
static bool detached;
static unsigned generation;
static bool attached;
extern void RsSret3(void* output, void* function, uintptr_t a, uintptr_t b, uintptr_t c);
__asm__(".text\n.balign 4\n.global RsSret3\n.type RsSret3,%function\n"
        "RsSret3:\n stp x29,x30,[sp,#-16]!\n mov x29,sp\n mov x8,x0\n mov x16,x1\n"
        "mov x0,x2\n mov x1,x3\n mov x2,x4\n blr x16\n ldp x29,x30,[sp],#16\n ret\n"
        ".size RsSret3,.-RsSret3\n");

static uint32_t Fingerprint(const void* pointer, size_t count)
{
    const unsigned char* p = pointer;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < count; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

extern const void* RsMakeTransactionCallback(void);
extern bool RsHasCompletedTransaction(uint64_t sequence);
static uint64_t lastAddedMarker = UINT64_MAX;
static uint64_t lastBufferMarker = UINT64_MAX;
static bool AddTransactionMarker(void* handler)
{
    static union { long double alignment; unsigned char bytes[112]; } listener;
    static bool registered;
    void* ctor = dlsym(library, "_ZN4OHOS5Rosen26SurfaceTransactionListenerC2ENS_4sptrINS_13IRemoteObjectEEE");
    void* reg = dlsym(library, "_ZN4OHOS5Rosen26SurfaceTransactionListener31RegisterCommandCompleteCallBackENSt3__h8functionIFvmmNS2_5queueImNS2_5dequeImNS2_9allocatorImEEEEEEEEE");
    void* get = dlsym(library, "_ZN4OHOS5Rosen26SurfaceTransactionListener10GetCommandERm");
    void* add = dlsym(library, "_ZN4OHOS5Rosen20RSTransactionHandler10AddCommandERNSt3__h10unique_ptrINS0_9RSCommandENS2_14default_deleteIS4_EEEEbNS0_10FollowTypeEm");
    if (!ctor || !reg || !get || !add || Fingerprint(ctor, 228) != 0xeeb6d8a9 ||
        Fingerprint(reg, 1452) != 0xd110289e) {
        LOG("transaction_prerequisites ctor=%{public}d reg=%{public}d get=%{public}d add=%{public}d", ctor!=NULL,reg!=NULL,get!=NULL,add!=NULL);
        return false;
    }
    if (!registered) {
        // Pinned constructor writes through offset 104; context's remote sptr is
        // at 480. Both system objects are retained until this test process exits.
        ((void(*)(void*,void*))ctor)(&listener, (char*)context.object + 480);
        registered = ((bool(*)(void*,const void*))reg)(&listener, RsMakeTransactionCallback());
        LOG("transaction_listener_registered=%{public}d", registered);
        if (!registered) return false;
    }
    uint64_t sequence = UINT64_MAX;
    void* command = NULL;
    RsSret3(&command, get, (uintptr_t)&listener, (uintptr_t)&sequence, 0);
    if (!command) { LOG("transaction_marker_null"); return false; }
    ((void(*)(void*,void**,bool,int,uint64_t))add)(handler, &command, true, 0, 0);
    LOG("transaction_marker sequence=%{public}llu consumed=%{public}d", (unsigned long long)sequence, command==NULL);
    if (!command) lastAddedMarker = sequence;
    return command == NULL;
}

napi_value ProbeRsCreate(napi_env env, napi_callback_info info)
{
    (void)info;
    int result = -1;
    if (nodes[0].object || nodes[1].object) { LOG("create_skipped nodes_still_alive"); goto done; }
    library = dlopen("/system/lib64/librender_service_client.z.so", RTLD_NOW | RTLD_NOLOAD);
    if (!library) { LOG("create_blocked library_unavailable"); goto done; }
    void* create = dlsym(library, "_ZN4OHOS5Rosen13RSSurfaceNode6CreateERKNS0_19RSSurfaceNodeConfigEbNSt3__h10shared_ptrINS0_11RSUIContextEEE");
    void* createFull = dlsym(library, "_ZN4OHOS5Rosen13RSSurfaceNode6CreateERKNS0_19RSSurfaceNodeConfigENS0_17RSSurfaceNodeTypeEbbNSt3__h10shared_ptrINS0_11RSUIContextEEE");
    void* constructor = dlsym(library, "_ZN4OHOS5Rosen13RSSurfaceNodeC2ERKNS0_19RSSurfaceNodeConfigEbNSt3__h10shared_ptrINS0_11RSUIContextEEE");
    void* managerFn = dlsym(library, "_ZN4OHOS5Rosen18RSUIContextManager15MutableInstanceEv");
    void* getContext = dlsym(library, "_ZNK4OHOS5Rosen18RSUIContextManager14GetRSUIContextEm");
    void* setDelegate = dlsym(library, "_ZN4OHOS5Rosen13RSSurfaceNode15SetDelegateModeEb");
    void* hardware = dlsym(library, "_ZN4OHOS5Rosen13RSSurfaceNode18SetHardwareEnabledEbNS0_19SelfDrawingNodeTypeEb");
    LOG("create_prerequisites factory=%{public}d manager=%{public}d context=%{public}d delegate=%{public}d",
        create != NULL, managerFn != NULL, getContext != NULL, setDelegate != NULL);
    if (!create || !createFull || !constructor || !managerFn || !getContext || !setDelegate || !hardware) goto done;
    if (Fingerprint(create, 232) != 0x801e5a23 || Fingerprint(createFull, 2168) != 0xea6a36b8 ||
        Fingerprint(constructor, 436) != 0xe95e87f1) {
        LOG("create_blocked firmware_fingerprint_mismatch"); goto done;
    }
    void* manager = ((void*(*)(void))managerFn)();
    if (!manager) goto done;
    // The inspected manager creates tokens as (creating thread ID << 32) | counter.
    // Query only this application's main-thread tokens; never inspect other apps.
    uint64_t token = 0;
    for (unsigned counter = 1; !context.object && counter <= 16; ++counter) {
        token = ((uint64_t)getpid() << 32) | counter;
        RsSret3(&context, getContext, (uintptr_t)manager, token, 0);
        if (context.object) break;
    }
    if (!context.object) { LOG("create_blocked no_main_thread_context"); goto done; }
    LOG("context_found token=%{public}llu", (unsigned long long)token);
    result = 0;
    detached = false;
    attached = false;
    ++generation;
    for (unsigned i = 0; i < 2; ++i) {
        // Pinned factory accesses: name[0..23], data@24, export@32, surfaceId@40,
        // sync@48, window type@49, context@56, skipCheck@72, nodeId@80.
        union { uint64_t align; unsigned char bytes[88]; } config = {0};
        const char* name = i ? "LumieProbeVideo" : "LumieProbeWeb";
        size_t len = strlen(name);
        config.bytes[0] = (unsigned char)(len << 1); // verified libc++ __h short string
        memcpy(config.bytes + 1, name, len);
        config.bytes[48] = 1;
        config.bytes[72] = 1;
        RsSret3(&nodes[i], create, (uintptr_t)&config, 0, (uintptr_t)&context);
        if (!nodes[i].object) { LOG("node_create layer=%{public}u ok=0", i); result = -2; break; }
        LOG("node_create layer=%{public}u ok=1 generation=%{public}u", i, generation);
        ((void(*)(void*,bool,int,bool))hardware)(nodes[i].object, true, 0, true);
        LOG("hardware_enabled layer=%{public}u", i);
        bool accepted = ((bool(*)(void*,bool))setDelegate)(nodes[i].object, true);
        LOG("delegate_mode layer=%{public}u accepted=%{public}d", i, accepted);
        if (!accepted) { result = -3; break; }
    }
    LOG("create_complete result=%{public}d visible_layers=0 buffers_submitted=0", result);
done:;
    napi_value value;
    napi_create_int32(env, result, &value);
    return value;
}

static bool PaintWindow(unsigned layer)
{
    void* getSurface = dlsym(library, "_ZNK4OHOS5Rosen13RSSurfaceNode10GetSurfaceEv");
    static void* surfaces[2];
    static OHNativeWindow* windows[2];
    if (!getSurface) return false;
    RsSret3(&surfaces[layer], getSurface, (uintptr_t)nodes[layer].object, 0, 0);
    if (!surfaces[layer]) return false;
    windows[layer] = OH_NativeWindow_CreateNativeWindow(&surfaces[layer]);
    OHNativeWindow* w = windows[layer];
    if (!w) return false;
    int rc = OH_NativeWindow_NativeWindowHandleOpt(w, SET_BUFFER_GEOMETRY, 320, 180);
    rc |= OH_NativeWindow_NativeWindowHandleOpt(w, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
    rc |= OH_NativeWindow_NativeWindowHandleOpt(w, SET_USAGE,
        (uint64_t)(NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_HW_TEXTURE | NATIVEBUFFER_USAGE_MEM_DMA));
    rc |= OH_NativeWindow_NativeWindowHandleOpt(w, SET_TIMEOUT, (int64_t)0);
    if (rc) { LOG("paint_configuration layer=%{public}u rc=%{public}d", layer, rc); return false; }
    OHNativeWindowBuffer* buffer = NULL;
    int fence = -1;
    rc = OH_NativeWindow_NativeWindowRequestBuffer(w, &buffer, &fence);
    if (rc || !buffer) { LOG("paint_request layer=%{public}u rc=%{public}d", layer, rc); return false; }
    if (fence >= 0) {
        struct pollfd p = { .fd = fence, .events = POLLIN };
        int ready = poll(&p, 1, 1000);
        close(fence);
        if (ready <= 0) { OH_NativeWindow_NativeWindowAbortBuffer(w, buffer); return false; }
    }
    OH_NativeBuffer* native = NULL;
    void* mapped = NULL;
    rc = OH_NativeBuffer_FromNativeWindowBuffer(buffer, &native);
    if (!rc) rc = OH_NativeBuffer_Map(native, &mapped);
    if (rc || !mapped) { OH_NativeWindow_NativeWindowAbortBuffer(w, buffer); return false; }
    OH_NativeBuffer_Config config;
    OH_NativeBuffer_GetConfig(native, &config);
    for (int y = 0; y < config.height; ++y) {
        uint32_t* row = (uint32_t*)((unsigned char*)mapped + y * config.stride);
        for (int x = 0; x < config.width; ++x) {
            bool grid = (x % 40 < 3) || (y % 30 < 3);
            row[x] = grid ? 0xffffffff : (layer ? 0xff00ddff : 0xffff6600);
        }
    }
    OH_NativeBuffer_Unmap(native);
    Region region = { .rects = NULL, .rectNumber = 0 };
    rc = OH_NativeWindow_NativeWindowFlushBuffer(w, buffer, -1, region);
    LOG("paint_flush layer=%{public}u rc=%{public}d stride=%{public}d", layer, rc, config.stride);
    return rc == 0;
}

extern const void* RsMakeBufferCallback(unsigned slot);
extern void RsTrackBuffer(unsigned slot, uint32_t sequence, void* node);
extern bool RsRegisterRelease(void* library, void* remoteArgument);
extern void RsPollBufferCallbacks(void);
extern int GpuFenceCreateDelayed(unsigned delayMs, OH_NativeBuffer** imageOut);
extern uint64_t GpuFenceNow(void);
extern void RsDiscardUnsubmittedCallback(unsigned slot);
extern bool RsTakeRetiredBuffer(unsigned slot);
typedef struct {
    OH_NativeBuffer* buffer;
    const void* callback;
    int fence;
    int producerFence;
    unsigned slot;
} PreparedPlane;
static unsigned nextSlot;
static OH_NativeBuffer* retained[12];
static int retainedProducerFence[12];
static void* setNativeBuffer;

static void DiscardPrepared(PreparedPlane planes[2])
{
    for (unsigned i = 0; i < 2; ++i) {
        if (planes[i].callback) RsDiscardUnsubmittedCallback(planes[i].slot);
        if (planes[i].fence >= 0) close(planes[i].fence);
        if (planes[i].producerFence >= 0) close(planes[i].producerFence);
        if (planes[i].buffer) OH_NativeBuffer_Unreference(planes[i].buffer);
        planes[i].fence = -1;
        planes[i].producerFence = -1;
        planes[i].buffer = NULL;
    }
}

// Prepare BOTH layers before staging any geometry or buffer command. A failed
// allocation/import/fence duplication must not leak a partial batch to ArkUI's
// next implicit flush. This is a local preflight, not server-side rollback.
static bool PrepareNative(PreparedPlane planes[2], int acquireFence,
    bool newFrame, OH_NativeBuffer* preparedVideo, bool failSecond)
{
    if (nextSlot + 2 > 12) return false;
    setNativeBuffer = dlsym(library, "_ZN4OHOS5Rosen13RSSurfaceNode9SetBufferENS_4sptrINS_13SurfaceBufferEEENS_18UniqueFdAddDeletorINS_14DefaultDeleterEEERKNSt3__h8functionIFviEEE");
    void* cbAbi = dlsym(library, "_ZN4OHOS5Rosen13RSSurfaceNode26SetBufferAvailableCallbackENSt3__h8functionIFvvEEE");
    void* identity = dlsym(library, "_ZN4OHOS17SurfaceBufferImpl27SurfaceBufferToNativeBufferEv");
    uint32_t insn = 0;
    if (identity) memcpy(&insn, identity, 4);
    if (!setNativeBuffer || !cbAbi || Fingerprint(cbAbi, 1588) != 0x5290b89e || insn != 0xd65f03c0) {
        LOG("native_buffer_abi_rejected set=%{public}d identity=%{public}x", setNativeBuffer!=NULL, insn);
        return false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (i == 1 && failSecond) {
            LOG("injected_prepare_failure layer=1 staged_commands=0");
            return false;
        }
        PreparedPlane* plane = &planes[i];
        plane->slot = nextSlot + i;
        OH_NativeBuffer_Config conf = { .width = 320, .height = 180,
            .format = NATIVEBUFFER_PIXEL_FMT_RGBA_8888,
            .usage = NATIVEBUFFER_USAGE_CPU_WRITE | NATIVEBUFFER_USAGE_HW_TEXTURE | NATIVEBUFFER_USAGE_MEM_DMA };
        plane->buffer = i && preparedVideo ? preparedVideo : OH_NativeBuffer_Alloc(&conf);
        OH_NativeBuffer* b = plane->buffer;
        if (!b) return false;
        if (i && preparedVideo) {
            OH_NativeBuffer_Reference(b);
        } else {
            void* pixels = NULL;
            if (OH_NativeBuffer_Map(b, &pixels) || !pixels) return false;
            OH_NativeBuffer_GetConfig(b, &conf);
            for (int y = 0; y < conf.height; ++y) {
                uint32_t* row = (void*)((char*)pixels + y * conf.stride);
                for (int x = 0; x < conf.width; ++x)
                    row[x] = x % 40 < 3 || y % 30 < 3 ? 0xffffffff :
                        (newFrame ? (i ? 0xffdd00dd : 0xff22cc00) : (i ? 0xff00ddff : 0xffff6600));
            }
            if (OH_NativeBuffer_Unmap(b)) return false;
        }
        plane->callback = RsMakeBufferCallback(plane->slot);
        if (!plane->callback) return false;
        plane->fence = i && acquireFence >= 0 ? dup(acquireFence) : -1;
        if (i && acquireFence >= 0 && plane->fence < 0) return false;
        plane->producerFence = i && acquireFence >= 0 ? dup(acquireFence) : -1;
        if (i && acquireFence >= 0 && plane->producerFence < 0) return false;
    }
    return true;
}

static void SubmitPrepared(unsigned layer, PreparedPlane* plane, bool newFrame)
{
    OH_NativeBuffer* b = plane->buffer;
    retained[plane->slot] = b;
    retainedProducerFence[plane->slot] = plane->producerFence;
    plane->producerFence = -1;
    RsTrackBuffer(plane->slot, OH_NativeBuffer_GetSeqNum(b), nodes[layer].object);
    void* bufferArgument = b;
    int submittedFence = plane->fence;
    OH_NativeBuffer_Reference(b);
    ((void(*)(void*,void*,void*,const void*))setNativeBuffer)(nodes[layer].object,
        &bufferArgument, &plane->fence, plane->callback);
    if (plane->fence >= 0) close(plane->fence);
    OH_NativeBuffer_Unreference(b);
    LOG("native_set_buffer layer=%{public}u slot=%{public}u sequence=%{public}u new_frame=%{public}d acquire_fence=%{public}d monotonic_ns=%{public}llu",
        layer, plane->slot, OH_NativeBuffer_GetSeqNum(b), newFrame, submittedFence,
        (unsigned long long)GpuFenceNow());
    plane->fence = -1;
    plane->buffer = NULL;
    ++nextSlot;
}

// Pinned libc++ __h control block: strong owners-minus-one at +8; the
// zero-strong virtual method at vtable +16. Verified in RSNode::RemoveChild and
// RSSurfaceNode::~RSSurfaceNode machine code. Use the system weak-release
// function; never call the node destructor directly or construct an app owner.
static bool DropNodeReference(SystemShared* ref)
{
    void* weak = dlsym(library, "_ZNSt3__h19__shared_weak_count14__release_weakEv");
    void* destructor = dlsym(library, "_ZN4OHOS5Rosen13RSSurfaceNodeD2Ev");
    if (!ref->control || !weak || !destructor || Fingerprint(destructor, 1576) != 0x7194ed42) {
        LOG("node_drop_blocked weak_release=%{public}d", weak != NULL); return false;
    }
    void* control = ref->control;
    long* owners = (long*)((char*)control + 8);
    long before = __atomic_load_n(owners, __ATOMIC_ACQUIRE);
    if (before < 0 || before > 64) return false;
    *ref = (SystemShared){0};
    if (__atomic_fetch_sub(owners, 1, __ATOMIC_ACQ_REL) == 0) {
        void** vtable = *(void***)control;
        ((void(*)(void*))vtable[2])(control);
        ((void(*)(void*))weak)(control);
    }
    LOG("node_owner_dropped previous_owners_minus_one=%{public}ld", before);
    return true;
}

static bool FlushLifetime(void)
{
    void* ctor = dlsym(library, "_ZN4OHOS5Rosen11RSUIContextC2EmRNS_4sptrINS_13IRemoteObjectEEE");
    void* flush = dlsym(library, "_ZN4OHOS5Rosen20RSTransactionHandler24FlushImplicitTransactionEmRKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEbm");
    void* request = dlsym(library, "_ZN4OHOS5Rosen11RSUIContext20RequestVsyncCallbackEv");
    if (!context.object || !ctor || Fingerprint(ctor, 1708) != 0x14d660fd || !flush || !request) return false;
    void* handler; memcpy(&handler, (char*)context.object + 120, sizeof(handler));
    if (!handler || !AddTransactionMarker(handler)) return false;
    uint64_t emptyString[3] = {0};
    ((void(*)(void*,uint64_t,const void*,bool,uint64_t))flush)(handler, 0, emptyString, false, 0);
    ((void(*)(void*))request)(context.object);
    return true;
}

// GetSurface returns an owning system sptr. Surface inherits RefBase at offset
// zero; release that one temporary owner through the system implementation.
static int ProbeSurface(unsigned layer, bool background)
{
    void* get = dlsym(library, "_ZNK4OHOS5Rosen13RSSurfaceNode10GetSurfaceEv");
    void* surfaceLib = dlopen("/system/lib64/chipset-pub-sdk/libsurface.z.so", RTLD_NOW | RTLD_NOLOAD);
    if (!get || !surfaceLib) { LOG("surface_lookup_failed getter=%{public}d library=%{public}d", get!=NULL,surfaceLib!=NULL); if (surfaceLib) dlclose(surfaceLib); return -1; }
    void* drop = dlsym(surfaceLib, "_ZN4OHOS7RefBase12DecStrongRefEPKv");
    void* go = dlsym(surfaceLib, "_ZN4OHOS15ProducerSurface12GoBackgroundEv");
    void* query = dlsym(surfaceLib, "_ZNK4OHOS15ProducerSurface19GetPresentTimestampEjNS_27GraphicPresentTimestampTypeERl");
    if (!drop || (background ? !go : !query)) {
        LOG("surface_api_missing drop=%{public}d background=%{public}d query=%{public}d", drop!=NULL,go!=NULL,query!=NULL);
        dlclose(surfaceLib); return -1;
    }
    if (Fingerprint(get,264) != 0x108d9ce0 || (background && Fingerprint(go,92) != 0x2ef7d186)) { dlclose(surfaceLib); return -1; }
    void* surface = NULL;
    RsSret3(&surface, get, (uintptr_t)nodes[layer].object, 0, 0);
    if (!surface) { LOG("surface_lookup_failed null_surface layer=%{public}u",layer); dlclose(surfaceLib); return -1; }
    int rc = 0;
    if (background) {
        rc = ((int(*)(void*))go)(surface);
        LOG("surface_background layer=%{public}u rc=%{public}d", layer, rc);
    } else {
        unsigned slot = nextSlot - 2 + layer;
        if (nextSlot < 2 || !retained[slot]) rc = -1;
        else for (int type = 1; type <= 2; ++type) {
            int64_t value = -1;
            uint32_t seq = OH_NativeBuffer_GetSeqNum(retained[slot]);
            int queryRc = ((int(*)(void*,uint32_t,int,int64_t*))query)(surface, seq, type, &value);
            LOG("present_query layer=%{public}u sequence=%{public}u type=%{public}d rc=%{public}d value=%{public}lld", layer,seq,type,queryRc,(long long)value);
            if (queryRc) rc = queryRc;
        }
    }
    ((void(*)(void*,const void*))drop)(surface, &surface);
    dlclose(surfaceLib);
    return rc;
}

static int ProbeLifetime(int phase)
{
    if (!library || !context.object) return -1;
    unsigned pending = 0;
    if (phase == 77) {
        if (!RsHasCompletedTransaction(lastBufferMarker)) {
            LOG("detach_deferred waiting_for_submit_marker=%{public}llu",(unsigned long long)lastBufferMarker);
            return 1;
        }
        int detachRc = ProbeLifetime(70);
        int backgroundRc = detachRc == 0 ? ProbeLifetime(74) : -1;
        int destroyRc = backgroundRc == 0 ? ProbeLifetime(72) : -1;
        LOG("inflight_teardown detach=%{public}d background=%{public}d destroy=%{public}d",detachRc,backgroundRc,destroyRc);
        return destroyRc;
    }
    if (phase == 74 || phase == 75) {
        if (!nodes[0].object || !nodes[1].object || (phase == 74 && !detached)) return -1;
        int first = ProbeSurface(0, phase == 74), second = ProbeSurface(1, phase == 74);
        if (phase == 74) FlushLifetime();
        return first || second ? -1 : 0;
    }
    if (phase == 71 || phase == 72) {
        for (unsigned i = 0; i < nextSlot; ++i) {
            if (!retained[i]) continue;
            int producer = retainedProducerFence[i];
            if (producer >= 0) {
                struct pollfd p = {producer, POLLIN, 0};
                if (poll(&p,1,0) != 1 || !(p.revents & POLLIN)) {
                    LOG("producer_pending slot=%{public}u monotonic_ns=%{public}llu",i,(unsigned long long)GpuFenceNow());
                    ++pending; continue;
                }
                close(producer); retainedProducerFence[i] = -1;
            }
            if (RsTakeRetiredBuffer(i)) {
                uint32_t sequence = OH_NativeBuffer_GetSeqNum(retained[i]);
                OH_NativeBuffer_Unreference(retained[i]); retained[i] = NULL;
                LOG("allocation_unreferenced slot=%{public}u sequence=%{public}u source=server_release monotonic_ns=%{public}llu", i, sequence,(unsigned long long)GpuFenceNow());
            } else ++pending;
        }
        LOG("drain_pending=%{public}u", pending);
        if (phase == 71 || pending) return pending ? 1 : 0;
        if (!detached) { LOG("destroy_blocked still_attached"); return -1; }
        for (unsigned i = 0; i < 2; ++i) {
            if (nodes[i].object && !DropNodeReference(&nodes[i])) return -1;
        }
        bool flushed = FlushLifetime();
        LOG("destroy_submitted generation=%{public}u flush=%{public}d", generation, flushed);
        return flushed ? 0 : -1;
    }
    if (phase != 70 || detached || !root.object || !nodes[0].object || !nodes[1].object) return -1;
    if (!RsHasCompletedTransaction(lastBufferMarker)) {
        LOG("detach_deferred waiting_for_submit_marker=%{public}llu",(unsigned long long)lastBufferMarker);
        return 1;
    }
    void* remove = dlsym(library, "_ZN4OHOS5Rosen6RSNode11RemoveChildENSt3__h10shared_ptrIS1_EE");
    void* clean = dlsym(library, "_ZN4OHOS5Rosen13RSSurfaceNode11CleanBufferEb");
    void* visible = dlsym(library, "_ZN4OHOS5Rosen6RSNode10SetVisibleEb");
    if (!remove || !clean || !visible || Fingerprint(remove,1176) != 0xd791aac8 ||
        Fingerprint(clean,20) != 0x0cd5d9fd) return -1;
    for (unsigned i = 0; i < 2; ++i) {
        ((void(*)(void*,bool))visible)(nodes[i].object, false);
        ((void(*)(void*,SystemShared*))remove)(root.object, &nodes[i]);
        ((void(*)(void*,bool))clean)(nodes[i].object, true);
    }
    detached = true;
    bool flushed = FlushLifetime();
    LOG("detach_clean_submitted generation=%{public}u flush=%{public}d", generation, flushed);
    return flushed ? 0 : -1;
}

napi_value ProbeRsShow(napi_env env, napi_callback_info info)
{
    napi_value argv[1]; size_t argc = 1; int32_t phase = 0, result = -1;
    int delayedFence = -1;
    OH_NativeBuffer* preparedVideo = NULL;
    PreparedPlane planes[2] = {{.fence=-1,.producerFence=-1}, {.fence=-1,.producerFence=-1}};
    bool newFrame = false;
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    if (argc) napi_get_value_int32(env, argv[0], &phase);
    if (phase >= 70 && phase <= 77) { result = ProbeLifetime(phase); LOG("lifetime_result phase=%{public}d result=%{public}d",phase,result); goto done; }
    if (phase == 69) {
        delayedFence = GpuFenceCreateDelayed(500, NULL);
        result = delayedFence >= 0 ? 0 : -1;
        goto done;
    }
    if (!nodes[0].object || !nodes[1].object || !context.object || detached) goto done;
    newFrame = (phase >= 60 && phase <= 63) || phase == 66;
    if (phase == 60 || phase == 61 || phase == 63 || phase == 66) {
        delayedFence = GpuFenceCreateDelayed(500, &preparedVideo);
        if (delayedFence < 0) { LOG("delayed_batch_aborted no_unsignaled_gpu_fence"); goto done; }
    }
    if (phase >= 10 && !PrepareNative(planes, delayedFence, newFrame, preparedVideo, phase == 62)) {
        LOG("batch_prepare_aborted phase=%{public}d staged_commands=0", phase);
        goto done;
    }
    if (phase >= 40) {
        void* ctor = dlsym(library, "_ZN4OHOS5Rosen11RSUIContextC2EmRNS_4sptrINS_13IRemoteObjectEEE");
        if (!ctor || Fingerprint(ctor, 1708) != 0x14d660fd) goto done;
        RsRegisterRelease(library, (char*)context.object + 480);
    }
    if (phase >= 50) {
        static bool checkedFence;
        if (!checkedFence) {
            checkedFence = true;
            int fd = open("/dev/sw_sync", O_RDWR | O_CLOEXEC);
            LOG("software_fence_device available=%{public}d errno=%{public}d", fd >= 0, fd < 0 ? errno : 0);
            if (fd >= 0) close(fd);
        }
    }
    void* getNode = dlsym(library, "_ZNK4OHOS5Rosen11RSNodeMapV27GetNodeINS0_6RSNodeEEEKNSt3__h10shared_ptrIT_EEm");
    void* add = dlsym(library, "_ZN4OHOS5Rosen6RSNode8AddChildENSt3__h10shared_ptrIS1_EEi");
    void* bounds = dlsym(library, "_ZN4OHOS5Rosen6RSNode9SetBoundsEffff");
    void* frame = dlsym(library, "_ZN4OHOS5Rosen6RSNode8SetFrameEffff");
    void* gravity = dlsym(library, "_ZN4OHOS5Rosen6RSNode15SetFrameGravityENS0_7GravityE");
    void* zOrder = dlsym(library, "_ZN4OHOS5Rosen6RSNode12SetPositionZEf");
    void* visible = dlsym(library, "_ZN4OHOS5Rosen6RSNode10SetVisibleEb");
    void* request = dlsym(library, "_ZN4OHOS5Rosen11RSUIContext20RequestVsyncCallbackEv");
    void* dst = dlsym(library, "_ZN4OHOS5Rosen13RSSurfaceNode18SetDelegateDstRectEffff");
    void* src = dlsym(library, "_ZN4OHOS5Rosen13RSSurfaceNode18SetDelegateSrcRectEffff");
    if (!getNode || !add || !bounds || !frame || !zOrder || !visible || !request || !dst || !src) {
        LOG("show_missing_api node=%{public}d add=%{public}d bounds=%{public}d visible=%{public}d vsync=%{public}d",
            getNode!=NULL,add!=NULL,bounds!=NULL,visible!=NULL,request!=NULL); goto done;
    }
    if (!root.object) {
        // Context map +40 is verified in the pinned Create() machine code.
        // Root id +32 is checked against the owning PID and the map before use.
        uint64_t rootId; memcpy(&rootId, (char*)context.object + 32, 8);
        if ((rootId >> 32) != (uint64_t)getpid()) { LOG("show_invalid_root_id"); goto done; }
        RsSret3(&root, getNode, (uintptr_t)context.object + 40, rootId, 0);
        if (!root.object) { LOG("show_root_not_found"); goto done; }
        // This firmware's app root has a single ArkUI canvas at rootId + 1,
        // confirmed in the device RSTree before running this occlusion control.
        RsSret3(&appCanvas, getNode, (uintptr_t)context.object + 40, rootId + 1, 0);
    }
    if (!attached) {
        for (unsigned i = 0; i < 2; ++i) {
            ((void(*)(void*,SystemShared*,int))add)(root.object, &nodes[i], -1);
            if (phase < 10 && !PaintWindow(i)) { LOG("show_paint_failed layer=%{public}u", i); goto done; }
        }
    }
    attached = true;
    if (appCanvas.object) {
        ((void(*)(void*,bool))visible)(appCanvas.object, phase < 20 || phase >= 30);
        LOG("app_canvas_visible=%{public}d", phase < 20 || phase >= 30);
    }
    for (unsigned i = 0; i < 2; ++i) {
        float x = i ? 320 : 100, y = i ? 650 : 450;
        int move = phase % 10;
        if (move == 1 || move == 3) { x += 100; y += 200; }
        float width = i ? 600 : 1000, height = i ? 340 : 700;
        if (phase == 63) { width *= 0.75f; height *= 0.75f; }
        ((void(*)(void*,float,float,float,float))bounds)(nodes[i].object, x, y, width, height);
        ((void(*)(void*,float,float,float,float))frame)(nodes[i].object, x, y, width, height);
        if (gravity && phase >= 50) ((void(*)(void*,int))gravity)(nodes[i].object, 5);
        ((void(*)(void*,float))zOrder)(nodes[i].object, 1000.0f + i);
        ((void(*)(void*,float,float,float,float))dst)(nodes[i].object, x, y, width, height);
        ((void(*)(void*,float,float,float,float))src)(nodes[i].object, 0, 0, 320, 180);
        ((void(*)(void*,bool))visible)(nodes[i].object, move != 2 && move != 4);
        if (phase >= 10) SubmitPrepared(i, &planes[i], newFrame);
    }
    RsPollBufferCallbacks();
    void* ctxCtor = dlsym(library, "_ZN4OHOS5Rosen11RSUIContextC2EmRNS_4sptrINS_13IRemoteObjectEEE");
    void* flush = dlsym(library, "_ZN4OHOS5Rosen20RSTransactionHandler24FlushImplicitTransactionEmRKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEbm");
    if (ctxCtor && Fingerprint(ctxCtor, 1708) == 0x14d660fd && flush) {
        void* handler; memcpy(&handler, (char*)context.object + 120, sizeof(handler));
        uint64_t emptyString[3] = {0};
        if (handler) {
            if (phase >= 40 && !AddTransactionMarker(handler)) goto done;
            if (phase >= 40) lastBufferMarker = lastAddedMarker;
            ((void(*)(void*,uint64_t,const void*,bool,uint64_t))flush)(handler, 0, emptyString, false, 0);
            LOG("explicit_context_flush phase=%{public}d monotonic_ns=%{public}llu", phase, (unsigned long long)GpuFenceNow());
        }
    } else { LOG("flush_unavailable symbol=%{public}d", flush != NULL); }
    ((void(*)(void*))request)(context.object);
    result = 0;
    LOG("show_submitted phase=%{public}d visibility=%{public}d", phase, phase % 10 != 2 && phase % 10 != 4);
    if (phase == 66) {
        int initial = ProbeLifetime(77);
        LOG("initial_teardown_attempt result=%{public}d", initial);
    }
done:;
    DiscardPrepared(planes);
    if (delayedFence >= 0) close(delayedFence);
    if (preparedVideo) OH_NativeBuffer_Unreference(preparedVideo);
    napi_value value; napi_create_int32(env, result, &value); return value;
}
