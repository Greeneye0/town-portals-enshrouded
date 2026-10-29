#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include "greeneye.h"
#include "portal_default_images.h"
#include "portal_default_view.h"
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#pragma comment(lib,"windowscodecs.lib")
#pragma comment(lib,"ole32.lib")

namespace portal::defaults {
namespace {
constexpr const wchar_t* kRegionFiles[kRegions]={L"springlands",L"revelwood",L"blackmire",L"nomad",
    L"kindlewastes",L"albaneve",L"veilwater",L"cinders"};
std::mutex g_mutex;std::condition_variable g_wake;
// Joined and deleted in Stop (hot reload and unload), like the sharing worker.
std::thread* g_worker{};bool g_stop{};
int g_region=-1;bool g_night{};int g_state=-1;bool g_pending{};
std::shared_ptr<sharing::Image> g_image;

void Log(const char* line) {if(auto* host=greeneye::Host())host->log(line);}
uint16_t Half(float f) {
    // Positive, finite and below 65504 here; rounds to nearest.
    uint32_t bits;std::memcpy(&bits,&f,4);
    const int exponent=int((bits>>23)&255)-127+15;
    if(exponent<=0)return 0;
    if(exponent>=31)return 0x7bff;
    const uint32_t mantissa=(bits&0x7fffff)+0x1000;
    return uint16_t((exponent<<10)+(mantissa>>13));
}
std::shared_ptr<sharing::Image> Decode(int region,bool night) {
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);
    if(auto* slash=wcsrchr(path,L'\\'))slash[1]=0;
    wchar_t name[64]{};std::swprintf(name,64,L"%s-%s.jpg",kRegionFiles[region],night?L"night":L"day");
    wcscat_s(path,L"greeneye\\town-portal\\biomes\\");wcscat_s(path,name);
    std::vector<uint8_t> rgba;uint32_t width{},height{};
    if(!LoadRgba(path,width,height,rgba,sharing::kWidth,sharing::kHeight)||width!=sharing::kWidth||height!=sharing::kHeight){
        char line[160]{};std::snprintf(line,sizeof(line),"Town Portal: default picture %ls not loaded (missing or not an image)",name);
        Log(line);return nullptr;
    }
    uint16_t light[256];for(int v=0;v<256;++v)light[v]=Half(SceneLight(uint8_t(v)));
    const uint16_t depth=Half(defaultview::kDepth);
    auto image=std::make_shared<sharing::Image>();image->pixels.resize(sharing::kRawBytes);
    const uint8_t* in=rgba.data();uint8_t* out=image->pixels.data();
    for(size_t i=0;i<size_t(width)*height;++i,in+=4,out+=8){
        const uint16_t px[4]={light[in[0]],light[in[1]],light[in[2]],depth};
        std::memcpy(out,px,8);
    }
    return image;
}
void Work() {
    for(;;){
        int region;bool night;
        {
            std::unique_lock lock(g_mutex);
            g_wake.wait(lock,[]{return g_stop||g_pending;});
            if(g_stop)break;
            region=g_region;night=g_night;g_pending=false;
        }
        auto image=Decode(region,night);
        std::lock_guard lock(g_mutex);
        if(region==g_region&&night==g_night&&!g_pending){g_image=std::move(image);g_state=g_image?1:-1;}
    }
}
}
bool LoadRgba(const wchar_t* path,uint32_t& width,uint32_t& height,std::vector<uint8_t>& rgba,uint32_t fitWidth,uint32_t fitHeight) {
    // S_OK / S_FALSE: this call initialised COM and must balance it; a thread already in
    // another apartment (RPC_E_CHANGED_MODE) can use WIC as it is.
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    IWICImagingFactory* factory{};IWICBitmapDecoder* decoder{};IWICBitmapFrameDecode* frame{};IWICFormatConverter* converter{};IWICBitmapScaler* scaler{};
    UINT w{},h{};
    bool ok=SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))&&
        SUCCEEDED(factory->CreateDecoderFromFilename(path,nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder))&&
        SUCCEEDED(decoder->GetFrame(0,&frame))&&SUCCEEDED(frame->GetSize(&w,&h))&&w&&h&&w<=8192&&h<=8192&&
        SUCCEEDED(factory->CreateFormatConverter(&converter));
    IWICBitmapSource* source=frame;
    if(ok&&fitWidth&&fitHeight&&(w!=fitWidth||h!=fitHeight)){
        ok=SUCCEEDED(factory->CreateBitmapScaler(&scaler))&&
            SUCCEEDED(scaler->Initialize(frame,fitWidth,fitHeight,WICBitmapInterpolationModeHighQualityCubic));
        source=scaler;w=fitWidth;h=fitHeight;
    }
    ok=ok&&SUCCEEDED(converter->Initialize(source,GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    if(ok){rgba.resize(size_t(w)*h*4);ok=SUCCEEDED(converter->CopyPixels(nullptr,w*4,UINT(rgba.size()),rgba.data()));}
    if(converter)converter->Release();if(scaler)scaler->Release();if(frame)frame->Release();if(decoder)decoder->Release();if(factory)factory->Release();
    if(SUCCEEDED(com))CoUninitialize();
    width=w;height=h;
    return ok;
}
float SceneLight(uint8_t srgb) {
    const float c=srgb/255.f;
    const float linear=c<=0.04045f?c/12.92f:std::pow((c+0.055f)/1.055f,2.4f);
    const float shown=std::min(linear,0.92f);
    return shown/(1.f-shown);
}
void Request(int region,bool night) {
    if(region<0||region>=kRegions)return;
    std::lock_guard lock(g_mutex);
    if(!g_worker){g_stop=false;g_worker=new std::thread(Work);}
    g_region=region;g_night=night;g_state=0;g_pending=true;g_image.reset();
    g_wake.notify_all();
}
int Take(int region,bool night,std::shared_ptr<sharing::Image>& image) {
    std::lock_guard lock(g_mutex);
    if(region!=g_region||night!=g_night)return -1;
    if(g_state==1){image=std::move(g_image);g_state=-1;return 1;}
    return g_state;
}
void Stop() {
    {std::lock_guard lock(g_mutex);g_stop=true;}g_wake.notify_all();
    if(g_worker){if(g_worker->joinable())g_worker->join();delete g_worker;g_worker=nullptr;}
    std::lock_guard lock(g_mutex);g_state=-1;g_image.reset();g_region=-1;
}
}
