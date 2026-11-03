// Debug-only driver for the production C bridge. Actual AVCodec Surface output
// is submitted without CPU video mapping/copy. Primary is a generated alpha
// test layer; this is not Chromium overlay promotion or a webpage screenshot.
#include "ohos_overlay_bridge.h"
#include "hdr_buffer_probe.h"
#include <atomic>
#include <deque>
#include <dlfcn.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <napi/native_api.h>
#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <native_window/external_window.h>
#include <multimedia/player_framework/native_avsource.h>
#include <multimedia/player_framework/native_avdemuxer.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avformat.h>

#define LOG(...) OH_LOG_Print(LOG_APP, LOG_INFO, 0xD003F01, "LumieRealVideo", __VA_ARGS__)
extern "C" void OhosRsSret3(void*, void*, uintptr_t, uintptr_t, uintptr_t);
namespace {
struct CodecBuffer { uint32_t index; OH_AVBuffer* buffer; };
struct Player;
struct VideoLease { Player* owner; OHNativeWindowBuffer* window; int fence; };
struct Player {
  OhosOverlayHost* host = nullptr;
  OH_AVSource* source = nullptr;
  OH_AVDemuxer* demuxer = nullptr;
  OH_AVCodec* codec = nullptr;
  OH_NativeImage* image = nullptr;
  OHNativeWindow* window = nullptr;
  std::mutex mutex;
  std::deque<CodecBuffer> inputs, outputs;
  std::atomic<int> error{0};
  bool input_eos = false, output_eos = false, closing = false;
  unsigned submitted = 0, released = 0, outstanding = 0;
  VideoLease* pending = nullptr;
  OH_NativeBuffer* pending_web = nullptr;
  uint32_t track = 0;
  int width = 0, height = 0;
  static void OnError(OH_AVCodec*, int32_t code, void* user) {
    static_cast<Player*>(user)->error.store(code ? code : -100);
  }
  static void OnFormat(OH_AVCodec*, OH_AVFormat*, void*) {}
  static void OnInput(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* user) {
    auto& self = *static_cast<Player*>(user);
    std::lock_guard<std::mutex> lock(self.mutex); self.inputs.push_back({index,buffer});
  }
  static void OnOutput(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* user) {
    auto& self = *static_cast<Player*>(user);
    std::lock_guard<std::mutex> lock(self.mutex); self.outputs.push_back({index,buffer});
  }
  static void ReleaseVideo(void* ptr) {
    std::unique_ptr<VideoLease> lease(static_cast<VideoLease*>(ptr));
    auto& owner = *lease->owner;
    int rc = OH_NativeImage_ReleaseNativeWindowBuffer(owner.image, lease->window, lease->fence);
    if (rc != 0) { LOG("release_failed rc=%{public}d",rc); std::abort(); }
    OH_NativeWindow_NativeObjectUnreference(lease->window);
    --owner.outstanding; ++owner.released;
  }
  static void ReleaseWeb(void* ptr) { OH_NativeBuffer_Unreference(static_cast<OH_NativeBuffer*>(ptr)); }
  bool CreateHost() {
    // Harness only: the primary UI context's first token. The production host
    // takes an explicit token/root from its shell, not an ID scan.
    void* rs = dlopen("/system/lib64/librender_service_client.z.so",RTLD_NOW|RTLD_NOLOAD);
    if (!rs) return false;
    auto manager = reinterpret_cast<void*(*)()>(dlsym(rs,"_ZN4OHOS5Rosen18RSUIContextManager15MutableInstanceEv"));
    void* get = dlsym(rs,"_ZNK4OHOS5Rosen18RSUIContextManager14GetRSUIContextEm");
    auto weak = reinterpret_cast<void(*)(void*)>(dlsym(rs,"_ZNSt3__h19__shared_weak_count14__release_weakEv"));
    struct Shared { void* object; void* control; } context{};
    const uint64_t token = (static_cast<uint64_t>(gettid())<<32)|1;
    if (manager && get && weak) OhosRsSret3(&context,get,reinterpret_cast<uintptr_t>(manager()),token,0);
    if (context.object) {
      uint64_t root = 0; std::memcpy(&root,static_cast<char*>(context.object)+32,8);
      host = OhosOverlayCreate(OHOS_OVERLAY_BRIDGE_VERSION,token,root);
      auto* owners = reinterpret_cast<long*>(static_cast<char*>(context.control)+8);
      if (__atomic_fetch_sub(owners,1,__ATOMIC_ACQ_REL)==0) {
        auto** table=*static_cast<void***>(context.control);
        reinterpret_cast<void(*)(void*)>(table[2])(context.control); weak(context.control);
      }
    }
    dlclose(rs); return host;
  }
  bool Start(const std::string& uri) {
    if (!CreateHost()) { LOG("host_unavailable"); return false; }
    source = OH_AVSource_CreateWithURI(const_cast<char*>(uri.c_str()));
    if (!source) return false;
    demuxer = OH_AVDemuxer_CreateWithSource(source);
    if (!demuxer) return false;
    OH_AVFormat* format = OH_AVSource_GetTrackFormat(source,track);
    if (!format) return false;
    const char* mime = nullptr;
    bool valid = OH_AVFormat_GetStringValue(format,OH_MD_KEY_CODEC_MIME,&mime) && mime &&
      OH_AVFormat_GetIntValue(format,OH_MD_KEY_WIDTH,&width) &&
      OH_AVFormat_GetIntValue(format,OH_MD_KEY_HEIGHT,&height);
    if (!valid) { OH_AVFormat_Destroy(format); return false; }
    auto* cap = OH_AVCodec_GetCapabilityByCategory(mime,false,HARDWARE);
    const char* name = cap ? OH_AVCapability_GetName(cap) : nullptr;
    codec = name ? OH_VideoDecoder_CreateByName(name) : nullptr;
    LOG("decoder=%{public}s size=%{public}dx%{public}d",name?name:"none",width,height);
    if (!codec) { OH_AVFormat_Destroy(format); return false; }
    OH_AVCodecCallback callbacks{OnError,OnFormat,OnInput,OnOutput};
    int rc = OH_VideoDecoder_RegisterCallback(codec,callbacks,this);
    OH_AVFormat_SetIntValue(format,OH_MD_KEY_PIXEL_FORMAT,AV_PIXEL_FORMAT_NV12);
    if (!rc) rc=OH_VideoDecoder_Configure(codec,format);
    OH_AVFormat_Destroy(format);
    image = OH_ConsumerSurface_Create();
    if (!image) return false;
    OH_OnFrameAvailableListener listener{nullptr,[](void*){}};
    if (!rc) rc=OH_NativeImage_SetOnFrameAvailableListener(image,listener);
    window = OH_NativeImage_AcquireNativeWindow(image);
    if (!window) return false;
    if (!rc) rc=OH_VideoDecoder_SetSurface(codec,window);
    if (!rc) rc=OH_AVDemuxer_SelectTrackByID(demuxer,track);
    if (!rc) rc=OH_VideoDecoder_Prepare(codec);
    if (!rc) rc=OH_VideoDecoder_Start(codec);
    LOG("start rc=%{public}d",rc); return rc==0;
  }
  void Close() {
    if (closing) return;
    closing=true;
    if (pending) { ReleaseVideo(pending); pending=nullptr; }
    if (pending_web) { ReleaseWeb(pending_web); pending_web=nullptr; }
    if (host) OhosOverlayClose(host);
  }
  bool Pump() {
    OhosOverlayStatus status{};
    if (host) OhosOverlayPoll(host,&status);
    if (error.load() && !closing) { LOG("codec_error=%{public}d",error.load()); Close(); }
    if (closing) {
      if (host && status.state!=3) return false;
      LOG("drained submitted=%{public}u released=%{public}u outstanding=%{public}u",submitted,released,outstanding);
      return true;
    }
    std::deque<CodecBuffer> feed;
    { std::lock_guard<std::mutex> lock(mutex); feed.swap(inputs); }
    for (auto& input:feed) {
      if (input_eos) break;
      int rc=OH_AVDemuxer_ReadSampleBuffer(demuxer,track,input.buffer);
      OH_AVCodecBufferAttr attr{};
      if (!rc) rc=OH_AVBuffer_GetBufferAttr(input.buffer,&attr);
      if (!rc) rc=OH_VideoDecoder_PushInputBuffer(codec,input.index);
      if (rc) {error.store(rc); return false;}
      input_eos=(attr.flags & AVCODEC_BUFFER_FLAGS_EOS)!=0;
    }
    // One codec output per UI tick: no blocking RenderOutputBuffer loop while
    // the decoder's ConsumerSurface leases are held by the display.
    if (!pending && outstanding<3) {
      CodecBuffer output{}; bool have=false;
      {std::lock_guard<std::mutex> lock(mutex); if(!outputs.empty()){output=outputs.front();outputs.pop_front();have=true;}}
      if (have) {
        OH_AVCodecBufferAttr attr{};
        int rc=OH_AVBuffer_GetBufferAttr(output.buffer,&attr);
        if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
          output_eos=true; OH_VideoDecoder_FreeOutputBuffer(codec,output.index);
        } else if (!rc) rc=OH_VideoDecoder_RenderOutputBuffer(codec,output.index);
        if(rc) {error.store(rc);return false;}
      }
    }
    if (!pending) {
      OHNativeWindowBuffer* frame=nullptr; int fence=-1;
      const int rc=OH_NativeImage_AcquireNativeWindowBuffer(image,&frame,&fence);
      if(!rc && frame) {
        if (OH_NativeWindow_NativeObjectReference(frame)!=0) std::abort();
        pending=new VideoLease{this,frame,fence}; ++outstanding;
      } else if(output_eos) {Close();return false;}
    }
    if (!pending) return false;
    OH_NativeBuffer* buffer=nullptr;
    if (OH_NativeBuffer_FromNativeWindowBuffer(pending->window,&buffer)!=0 || !buffer) {error.store(-101);return false;}
    if (!pending_web) {
      OH_NativeBuffer_Config config{width,height,NATIVEBUFFER_PIXEL_FMT_RGBA_8888,
        NATIVEBUFFER_USAGE_CPU_WRITE|NATIVEBUFFER_USAGE_HW_TEXTURE|NATIVEBUFFER_USAGE_MEM_DMA,0};
      pending_web=OH_NativeBuffer_Alloc(&config); if(!pending_web) {error.store(-102);return false;}
      void* data=nullptr;
      if(OH_NativeBuffer_Map(pending_web,&data)!=0) {error.store(-103);return false;}
      OH_NativeBuffer_GetConfig(pending_web,&config);
      for(int y=0;y<height;++y){auto* row=reinterpret_cast<uint32_t*>(static_cast<char*>(data)+y*config.stride);
        for(int x=0;x<width;++x) row[x]=(y<8 || y>=height-8 || x<8 || x>=width-8 || (y>height/2 && y<height/2+12))?0xffffffff:0;}
      OH_NativeBuffer_Unmap(pending_web);
      OH_NativeBuffer_SetColorSpace(pending_web,OH_COLORSPACE_SRGB_FULL);
    }
    if (submitted==0) {LumieInspectHdrBuffer(buffer,1,0,0);LumieInspectHdrBuffer(pending_web,1,0,1);}
    const OhosOverlayRect src{0,0,static_cast<float>(width),static_cast<float>(height)};
    const OhosOverlayRect dst{100,450,1100,1100.f*height/width};
    OhosOverlayFrame f{};f.version=OHOS_OVERLAY_BRIDGE_VERSION;f.has_video=1;f.primary_has_video_hole=1;
    f.primary={pending_web,-1,OHOS_OVERLAY_SDR_TAGS_PRISTINE,src,dst,pending_web,ReleaseWeb};
    // The fixture uses a new codec and ConsumerSurface for each file. No HDR
    // stream has previously used an SDR fixture's queue. Never set this flag
    // when reconfiguring a live HDR queue to SDR.
    f.video={buffer,pending->fence,OHOS_OVERLAY_SDR_TAGS_PRISTINE,src,dst,pending,ReleaseVideo};
    const auto rc=OhosOverlaySubmit(host,&f);
    if(rc==OHOS_OVERLAY_ACCEPTED){
      // Bridge duplicates the acquire fence. On retirement both producer and
      // display have finished, so the original consumer lease returns with -1.
      if(pending->fence>=0) close(pending->fence);pending->fence=-1;
      pending=nullptr;pending_web=nullptr;++submitted;
      if(submitted<=4 || submitted%30==0) LOG("submitted=%{public}u released=%{public}u",submitted,released);
    } else if(rc!=OHOS_OVERLAY_BUSY){LOG("submit_rejected rc=%{public}d",rc);error.store(-104);}
    return false;
  }
  ~Player() {
    if (host && !OhosOverlayDestroy(host)) std::abort();
    if (codec) {OH_VideoDecoder_Stop(codec);OH_VideoDecoder_Destroy(codec);}
    if (image) {OH_NativeImage_UnsetOnFrameAvailableListener(image);OH_NativeImage_Destroy(&image);}
    if (demuxer) OH_AVDemuxer_Destroy(demuxer);
    if (source) OH_AVSource_Destroy(source);
  }
};
std::unique_ptr<Player> player;
}
extern "C" napi_value StartRealOverlayVideo(napi_env env,napi_callback_info info) {
  napi_value args[1], result;size_t count=1; napi_get_cb_info(env,info,&count,args,nullptr,nullptr);
  char uri[1024]={};size_t length=0;
  int rc=-1;
  if(count==1 && !player && napi_get_value_string_utf8(env,args[0],uri,sizeof(uri),&length)==napi_ok){
    player=std::make_unique<Player>();rc=player->Start(uri)?0:-1;if(rc)player->Close();
  }
  napi_create_int32(env,rc,&result);return result;
}
extern "C" napi_value PumpRealOverlayVideo(napi_env env,napi_callback_info) {
  int rc=1;
  if(player){const bool done=player->Pump();rc=done?1:0;if(done)player.reset();}
  napi_value result;napi_create_int32(env,rc,&result);return result;
}
extern "C" napi_value StopRealOverlayVideo(napi_env env,napi_callback_info) {
  if(player)player->Close();napi_value result;napi_get_undefined(env,&result);return result;
}
