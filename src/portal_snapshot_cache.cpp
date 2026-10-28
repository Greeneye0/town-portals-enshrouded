#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "greeneye.h"
#include "portal_snapshot_cache.h"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace portal::homecache {
namespace {
struct Job {Key key{};std::shared_ptr<const sharing::Image> save;bool load{};};
std::mutex g_mutex;std::condition_variable g_wake;
// Joined and deleted in Stop (hot reload and unload), like the sharing worker.
std::thread* g_worker{};bool g_stop{};
std::deque<Job> g_jobs;
Key g_loadKey{};int g_loadState=-1;std::shared_ptr<sharing::Image> g_loaded;

void Path(const Key& key,wchar_t* path,const wchar_t* suffix=L"") {
    GetModuleFileNameW(nullptr,path,MAX_PATH);
    if(auto* slash=wcsrchr(path,L'\\'))slash[1]=0;
    wcscat_s(path,MAX_PATH,L"greeneye\\town-portal\\");
    wchar_t name[96]{};FileName(key,name,96);
    wcscat_s(path,MAX_PATH,name);wcscat_s(path,MAX_PATH,suffix);
}
void Log(const char* line) {if(auto* host=greeneye::Host())host->log(line);}

void SaveNow(const Key& key,const sharing::Image& image) {
    const auto bytes=EncodeFile(key,image);
    if(bytes.empty()){Log("Town Portal: home snapshot not saved (unusable pixels)");return;}
    // Written beside the file and then moved over it, so a crash never leaves half a file.
    wchar_t path[MAX_PATH]{},temp[MAX_PATH]{};Path(key,path);Path(key,temp,L".tmp");
    HANDLE file=CreateFileW(temp,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){Log("Town Portal: home snapshot not saved (cannot create the file)");return;}
    DWORD written{};const bool ok=WriteFile(file,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size();
    CloseHandle(file);
    if(!ok||!MoveFileExW(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        DeleteFileW(temp);Log("Town Portal: home snapshot not saved (write failed)");return;
    }
    char line[128]{};
    std::snprintf(line,sizeof(line),"Town Portal: home snapshot saved for the next login (%.1f MB)",double(bytes.size())/1048576.);
    Log(line);
}
std::shared_ptr<sharing::Image> LoadNow(const Key& key) {
    wchar_t path[MAX_PATH]{};Path(key,path);
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return nullptr;
    LARGE_INTEGER size{};std::vector<uint8_t> bytes;
    if(GetFileSizeEx(file,&size)&&size.QuadPart>0&&size.QuadPart<=LONGLONG(sizeof(FileHeader)+sharing::kMaxEncoded)){
        bytes.resize(size_t(size.QuadPart));DWORD read{};
        if(!ReadFile(file,bytes.data(),DWORD(bytes.size()),&read,nullptr)||read!=bytes.size())bytes.clear();
    }
    CloseHandle(file);
    auto image=std::make_shared<sharing::Image>();
    if(!DecodeFile(key,bytes,*image)){
        Log("Town Portal: home snapshot file is damaged, from an older version or another altar; ignored");return nullptr;
    }
    return image;
}
void Work() {
    for(;;){
        Job job;
        {
            std::unique_lock lock(g_mutex);
            g_wake.wait(lock,[]{return g_stop||!g_jobs.empty();});
            if(g_stop)return;
            job=std::move(g_jobs.front());g_jobs.pop_front();
        }
        if(job.save){SaveNow(job.key,*job.save);continue;}
        auto image=LoadNow(job.key);
        std::lock_guard lock(g_mutex);
        if(g_loadKey==job.key){g_loaded=std::move(image);g_loadState=g_loaded?1:-1;}
    }
}
void Queue(Job job) {
    std::lock_guard lock(g_mutex);
    if(!g_worker){g_stop=false;g_worker=new std::thread(Work);}
    g_jobs.push_back(std::move(job));g_wake.notify_all();
}
}
bool Exists(const Key& key) {
    if(!Valid(key))return false;
    wchar_t path[MAX_PATH]{};Path(key,path);
    return GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES;
}
void Save(const Key& key,std::shared_ptr<const sharing::Image> image) {
    if(!Valid(key)||!image)return;
    Queue({key,std::move(image),false});
}
void Load(const Key& key) {
    if(!Valid(key))return;
    {std::lock_guard lock(g_mutex);g_loadKey=key;g_loadState=0;g_loaded.reset();}
    Queue({key,nullptr,true});
}
int TakeLoaded(const Key& key,std::shared_ptr<sharing::Image>& image) {
    std::lock_guard lock(g_mutex);
    if(!(g_loadKey==key))return -1;
    if(g_loadState==1){image=std::move(g_loaded);g_loadState=-1;return 1;}
    return g_loadState;
}
void Stop() {
    {std::lock_guard lock(g_mutex);g_stop=true;g_jobs.clear();}g_wake.notify_all();
    if(g_worker){if(g_worker->joinable())g_worker->join();delete g_worker;g_worker=nullptr;}
    std::lock_guard lock(g_mutex);g_loadState=-1;g_loaded.reset();
}
}
