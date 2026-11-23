// SPDX-License-Identifier: GPL-3.0-only
#include "audio.hpp"
#include "ui.hpp"
#include "tag_writer.hpp"
#include "matroska.hpp"
#include "host_settings.hpp"
#include "language.hpp"
#include "winamp_input.hpp"
#include <process.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <cwchar>
#include <charconv>
#include <cmath>
#include <memory>
#include <mutex>
using namespace ogg_input;
namespace {
winamp::InModule module{};
std::unique_ptr<Audio> audio;
std::unique_ptr<Pcm> pcm;
HANDLE stop_event=nullptr,control_event=nullptr,thread=nullptr;
std::atomic<bool> output_open{false};bool vis_open=false;
std::atomic<bool> paused{false};std::atomic<int> seek_ms{-1},decoded_ms{0},length_ms{-1};
std::atomic<int> volume{-666},pan{0};
std::mutex lock,output_lock;
Settings configured,active;
TrackInfo current;
std::wstring current_path,last_error;
int active_channels=0,active_rate=0;
struct Cache{std::wstring path;TrackInfo info;WIN32_FILE_ATTRIBUTE_DATA attributes{};bool valid=false;} cache;
std::wstring staged_path;Tags staged_tags;
void Copy(wchar_t* target,int size,const std::wstring& value){if(target&&size>0)wcsncpy_s(target,size,value.c_str(),_TRUNCATE);}
int DisplayBitrate(int bitrate) noexcept {
    return std::max(1,static_cast<int>((static_cast<std::int64_t>(bitrate)+500)/1000));
}
Settings GetSettings(){std::lock_guard<std::mutex> guard(lock);return configured;}
TrackInfo GetInfo(const wchar_t* path){
    {std::lock_guard<std::mutex> guard(lock);if(!path||!*path||(active_channels&&!_wcsicmp(path,current_path.c_str())))return current;}
    if(IsHttp(path)){TrackInfo i;i.path=path;i.title=RequestUrl(path);i.radio=true;return i;}
    WIN32_FILE_ATTRIBUTE_DATA a{};bool valid=GetFileAttributesExW(path,GetFileExInfoStandard,&a)!=FALSE;
    {std::lock_guard<std::mutex> guard(lock);if(valid&&cache.valid&&!_wcsicmp(cache.path.c_str(),path)&&a.nFileSizeHigh==cache.attributes.nFileSizeHigh&&a.nFileSizeLow==cache.attributes.nFileSizeLow&&!CompareFileTime(&a.ftLastWriteTime,&cache.attributes.ftLastWriteTime))return cache.info;}
    Audio probe;std::wstring error;TrackInfo info;if(probe.Open(path,GetSettings(),false,error))info=probe.Info();
    {std::lock_guard<std::mutex> guard(lock);cache={path,info,a,valid};}return info;
}
bool Stopping(DWORD ms=0){return WaitForSingleObject(stop_event,ms)!=WAIT_TIMEOUT;}
void WaitForControl(){const HANDLE events[]{stop_event,control_event};WaitForMultipleObjects(2,events,FALSE,INFINITE);}
unsigned Run(){
    const int bytes=active.bits/8;
    std::array<std::uint32_t,BlockFrames*MaxChannels*2> storage{};
    std::array<short,BlockFrames*MaxChannels> visual{};
    auto* data=reinterpret_cast<unsigned char*>(storage.data());auto* out=module.outMod;
    int pending=0,offset=0,end_ms=0,retries=0;
    bool eof=false,notified=false;auto bitrate_at=GetTickCount64()+250;
    int displayed_bitrate=DisplayBitrate(audio->Info().current.bitrate);
    auto revision=audio->MetadataRevision();
    auto publish_metadata=[&]{
        if(revision==audio->MetadataRevision())return;
        revision=audio->MetadataRevision();const auto& info=audio->Info();bool title_changed;
        {
            std::lock_guard<std::mutex> guard(lock);
            title_changed=current.title!=info.title||current.current.tags!=info.current.tags;
            current=info;
        }
        const int length=info.radio?-1:info.duration_ms;
        if(length_ms.load()!=length)length_ms=length;
        if(title_changed&&module.hMainWindow)PostMessageW(module.hMainWindow,WM_USER,0,243);
    };
    while(!Stopping()){
        const int channels=pcm->Channels(),rate=pcm->Rate(),frame_bytes=bytes*channels;
        int request=seek_ms.exchange(-1);
        if(request>=0&&output_open&&audio->Seek(request)){pcm->Reset(request);pending=offset=0;eof=notified=false;out->Flush(request);decoded_ms=request;}
        if(paused){WaitForControl();continue;}
        if(pending){int n=std::min({pending,out->CanWrite(),8192});n-=n%frame_bytes;
            if(n>0&&!out->Write(reinterpret_cast<char*>(data)+offset,n)){pending-=n;offset+=n;if(!pending)decoded_ms=end_ms;}else Stopping(5);continue;}
        if(eof){if(notified){WaitForControl();continue;}if(output_open){out->CanWrite();if(!notified&&!out->IsPlaying()){if(module.hMainWindow)PostMessageW(module.hMainWindow,winamp::kEndOfFile,0,0);notified=true;}}else if(!notified){if(module.hMainWindow)PostMessageW(module.hMainWindow,winamp::kEndOfFile,0,0);notified=true;}Stopping(10);continue;}
        if(out->CanWrite()<frame_bytes){Stopping(5);continue;}
        int n=pcm->Read(data,BlockFrames);
        publish_metadata();
        if(n==Pcm::FormatChange){
            // Drain before closing. A pending seek supersedes the boundary.
            while(!Stopping()&&seek_ms.load()<0){out->CanWrite();if(!out->IsPlaying())break;Stopping(5);}
            if(Stopping())break;if(seek_ms.load()>=0)continue;
            const int boundary=pcm->PositionMs(),new_rate=pcm->NextRate();
            int new_channels=pcm->NextChannels();bool fallback=false;int latency=-1;
            {
                std::lock_guard<std::mutex> guard(output_lock);
                out->Close();output_open=false;
                latency=out->Open(new_rate,new_channels,active.bits,0,0);
                if(latency<0&&new_channels!=2){new_channels=2;fallback=true;latency=out->Open(new_rate,2,active.bits,0,0);}
                if(latency>=0){output_open=true;out->Flush(boundary);if(out->SetVolume)out->SetVolume(volume);if(out->SetPan)out->SetPan(pan);if(paused)out->Pause(1);}
            }
            if(latency<0||!pcm->AcceptFormat(fallback)){
                {std::lock_guard<std::mutex> guard(lock);last_error=L"The output plugin could not accept the next logical stream's audio format.";}
                eof=true;continue;
            }
            decoded_ms=pcm->PositionMs();
            if(vis_open){if(module.SAVSADeInit)module.SAVSADeInit();vis_open=false;}
            if(module.SAVSAInit){module.SAVSAInit(latency,new_rate);vis_open=true;}
            if(module.VSASetInfo)module.VSASetInfo(new_rate,new_channels);
            // A bitrate and rate in the same callback requests a playlist
            // title refresh in Winamp, even when only the output format changed.
            if(module.SetInfo)module.SetInfo(-1,new_rate/1000,new_channels,1);
            const int bitrate=DisplayBitrate(audio->Info().current.bitrate);
            if(bitrate!=displayed_bitrate&&module.SetInfo)module.SetInfo(bitrate,-1,-1,1);
            displayed_bitrate=bitrate;
            bitrate_at=GetTickCount64()+250;
            {std::lock_guard<std::mutex> guard(lock);active_channels=new_channels;active_rate=new_rate;}
            continue;
        }
        if(n<=0){
            bool connected=false;
            while(audio->Info().radio&&retries<active.reconnect_attempts&&!Stopping()){
                ++retries;if(Stopping(active.reconnect_delay_ms))break;auto path=audio->Info().path;auto retry_settings=active;retry_settings.rate=rate;
                if(channels==1)retry_settings.channels=ChannelMode::Mono;else if(channels==4)retry_settings.channels=ChannelMode::Quad;else retry_settings.channels=ChannelMode::Stereo;
                std::wstring error;if(audio->Open(path.c_str(),retry_settings,false,error)&&audio->Channels()==channels){pcm=std::make_unique<Pcm>(*audio,retry_settings);if(pcm->Ready()){pcm->Reset(decoded_ms);connected=true;break;}}
                {std::lock_guard<std::mutex> guard(lock);last_error=error;}
            }
            if(connected)continue;
            if(n<0&&!Stopping()){std::lock_guard<std::mutex> guard(lock);last_error=audio->Error().empty()?L"The audio stream could not be decoded.":audio->Error();}eof=true;continue;
        }
        retries=0;
        if(module.SAAddPCMData||module.VSAAddPCMData){
            memset(data+n*frame_bytes,active.bits==8?128:0,(BlockFrames-n)*frame_bytes);
            void* visuals=data;
            if(active.bits!=16){for(int i=0;i<BlockFrames*channels;++i){auto* p=data+i*bytes;visual[i]=active.bits==8?static_cast<short>((int(*p)-128)*256):static_cast<short>(p[bytes-2]|(p[bytes-1]<<8));}visuals=visual.data();}
            if(module.SAAddPCMData)module.SAAddPCMData(visuals,channels,16,decoded_ms);
            if(module.VSAAddPCMData)module.VSAAddPCMData(visuals,channels,16,decoded_ms);
        }
        int made=n;if(module.dsp_isactive&&module.dsp_dosamples&&module.dsp_isactive())made=module.dsp_dosamples(reinterpret_cast<short*>(data),n,active.bits,channels,rate);
        if(made<0||made>n*2){eof=true;continue;}pending=made*frame_bytes;offset=0;end_ms=pcm->PositionMs();if(!pending)decoded_ms=end_ms;
        if(active.instant_bitrate&&GetTickCount64()>=bitrate_at){
            const int br=audio->InstantBitrate();
            if(br>0&&module.SetInfo){
                const int kbps=DisplayBitrate(br);
                if(kbps!=displayed_bitrate){
                    // Sending bitrate together with sample rate requests a
                    // Winamp title refresh. Keep format changes separate;
                    // -1 preserves the existing sample rate and channel count.
                    module.SetInfo(kbps,-1,-1,1);displayed_bitrate=kbps;
                }
            }
            bitrate_at=GetTickCount64()+250;
        }
    }return 0;
}
unsigned __stdcall Worker(void*)noexcept{try{return Run();}catch(...){if(module.hMainWindow)PostMessageW(module.hMainWindow,winamp::kEndOfFile,0,0);return 1;}}
void __cdecl Stop(){
    if(thread){SetEvent(stop_event);if(audio)audio->Cancel();WaitForSingleObject(thread,INFINITE);CloseHandle(thread);thread=nullptr;}
    if(stop_event){CloseHandle(stop_event);stop_event=nullptr;}
    if(control_event){CloseHandle(control_event);control_event=nullptr;}
    if(output_open){module.outMod->Close();output_open=false;}if(vis_open){if(module.SAVSADeInit)module.SAVSADeInit();vis_open=false;}
    pcm.reset();audio.reset();paused=false;seek_ms=-1;decoded_ms=0;length_ms=-1;module.is_seekable=1;
    {std::lock_guard<std::mutex> guard(lock);active_channels=active_rate=0;current_path.clear();}
}
int __cdecl Play(const wchar_t* path){Stop();try{
    auto* out=module.outMod;if(!out||!out->Open||!out->Close||!out->Write||!out->CanWrite||!out->IsPlaying||!out->Pause||!out->Flush||!out->GetOutputTime||!out->GetWrittenTime)return 1;
    auto settings=ResolveHostSettings(module.hMainWindow,GetSettings());settings.interactive_auth=true;auto next=std::make_unique<Audio>();std::wstring error;
    bool shuffle=module.hMainWindow&&settings.gain==GainMode::Automatic&&SendMessageW(module.hMainWindow,WM_USER,0,250)!=0;
    if(!next->Open(path,settings,shuffle,error,true)){std::lock_guard<std::mutex> guard(lock);last_error=error;return IsHttp(path)?1:-1;}
    int latency=out->Open(next->Rate(),next->Channels(),settings.bits,0,0);
    if(latency<0&&next->Channels()!=2){settings.channels=ChannelMode::Stereo;if(!next->Open(path,settings,shuffle,error,true))return 1;latency=out->Open(next->Rate(),2,settings.bits,0,0);}
    if(latency<0)return 1;output_open=true;audio=std::move(next);active=settings;pcm=std::make_unique<Pcm>(*audio,settings);if(!pcm->Ready()){Stop();return 1;}
    stop_event=CreateEventW(nullptr,TRUE,FALSE,nullptr);control_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!stop_event||!control_event){Stop();return 1;}
    {std::lock_guard<std::mutex> guard(lock);current=audio->Info();current_path=path;active_channels=audio->Channels();active_rate=audio->Rate();last_error.clear();}
    module.is_seekable=audio->Info().seekable?1:0;length_ms=audio->Info().duration_ms;
    if(module.SAVSAInit){module.SAVSAInit(latency,audio->Rate());vis_open=true;}if(module.VSASetInfo)module.VSASetInfo(audio->Rate(),audio->Channels());
    if(module.SetInfo)module.SetInfo(DisplayBitrate(audio->Info().current.bitrate),audio->Rate()/1000,audio->Channels(),1);
    if(out->SetVolume)out->SetVolume(volume);if(out->SetPan)out->SetPan(pan);
    thread=reinterpret_cast<HANDLE>(_beginthreadex(nullptr,0,Worker,nullptr,0,nullptr));if(!thread){Stop();return 1;}return 0;
}catch(...){Stop();return 1;}}
int __cdecl IsOurFile(const wchar_t* path){if(!path||!*path)return 0;try{
    if(IsHttp(path)){std::wstring url(path);for(auto hint:{L">.ogg",L">.oga",L">.opus"}){auto n=wcslen(hint);if(url.size()>=n&&!_wcsicmp(url.c_str()+url.size()-n,hint))return 1;}
        // Winamp preloads in_vorbis: handle Ogg URL suffixes before any extension check.
        auto end=url.find_first_of(L"?#");url.resize(end==std::wstring::npos?url.size():end);auto dot=url.find_last_of('.');if(dot!=std::wstring::npos){auto ext=url.c_str()+dot;if(!_wcsicmp(ext,L".opus")||!_wcsicmp(ext,L".opu"))return 0;if(!_wcsicmp(ext,L".ogg")||!_wcsicmp(ext,L".oga")||!_wcsicmp(ext,L".spx"))return 1;}
    }else{auto ext=wcsrchr(path,L'.');if(!ext||(_wcsicmp(ext,L".ogg")&&_wcsicmp(ext,L".oga")&&_wcsicmp(ext,L".spx")))return 0;}
    ByteStream stream;std::wstring error;if(!stream.Open(path,ResolveHostSettings(module.hMainWindow,GetSettings()),error))return 0;return Probe(stream)!=Codec::Unknown;
}catch(...){return 0;}}
void __cdecl GetFileInfo(const wchar_t* path,wchar_t* title,int* duration){if(title)*title=0;if(duration)*duration=-1;try{auto i=GetInfo(path);Copy(title,winamp::kTitleCapacity,i.title);if(duration)*duration=i.duration_ms;}catch(...){}}
int __cdecl InfoBox(const wchar_t* path,HWND parent){try{auto i=GetInfo(path);if(i.codec==Codec::Unknown){std::wstring text=Localized(L"Cannot read this Ogg audio file.");{std::lock_guard<std::mutex> guard(lock);if(!last_error.empty())text+=L"\n\n"+last_error;}MessageBoxW(parent,text.c_str(),Localized(L"Ogg Audio Information").c_str(),MB_OK|MB_ICONERROR);}else {ShowInfo(module.hDllInstance,parent,i);std::lock_guard<std::mutex> guard(lock);cache={};}}catch(...){}return 0;}
void __cdecl Config(HWND parent){try{std::wstring status=Localized(L"Playback is stopped.");{std::lock_guard<std::mutex> guard(lock);if(active_channels)status=std::to_wstring(active_channels)+L" channels, "+std::to_wstring(active_rate)+L" Hz";}auto s=Configure(module.hDllInstance,parent,GetSettings(),status);{std::lock_guard<std::mutex> guard(lock);configured=s;}}catch(...){}}
void __cdecl About(HWND parent){ShowAbout(module.hDllInstance,parent);}
void __cdecl Init(){
    {std::lock_guard<std::mutex> guard(lock);configured=LoadSettings();}
    module.UsesOutputPlug|=8|16;
    RegisterMatroska(module.hMainWindow);
    InitializeLanguage(module.hMainWindow,module.hDllInstance);
}
void __cdecl Quit(){Stop();ShutdownLanguage();UnregisterMatroska();}
void __cdecl Pause(){paused=true;{std::lock_guard<std::mutex> guard(output_lock);if(output_open)module.outMod->Pause(1);}if(control_event)SetEvent(control_event);}
void __cdecl Unpause(){{std::lock_guard<std::mutex> guard(output_lock);if(output_open)module.outMod->Pause(0);}paused=false;if(control_event)SetEvent(control_event);}
int __cdecl Paused(){return paused?1:0;}
int __cdecl Length(){return length_ms;}
int __cdecl Time(){std::lock_guard<std::mutex> guard(output_lock);if(!output_open)return 0;auto n=static_cast<std::int64_t>(decoded_ms)+module.outMod->GetOutputTime()-module.outMod->GetWrittenTime();return static_cast<int>(std::clamp<std::int64_t>(n,0,length_ms>=0?length_ms.load():INT_MAX));}
void __cdecl Seek(int ms){if(output_open&&module.is_seekable){seek_ms=std::clamp(ms,0,length_ms>=0?length_ms.load():INT_MAX);if(control_event)SetEvent(control_event);}}
void __cdecl Volume(int v){volume=v;std::lock_guard<std::mutex> guard(output_lock);if(output_open&&module.outMod->SetVolume)module.outMod->SetVolume(v);}
void __cdecl Pan(int v){pan=v;std::lock_guard<std::mutex> guard(output_lock);if(output_open&&module.outMod->SetPan)module.outMod->SetPan(v);}
void __cdecl Eq(int,char[10],int){}
bool ParseGain(const wchar_t* input,double& db){
    auto text=ToUtf8(input);auto first=text.data();const auto end=first+text.size();
    while(first!=end&&(*first==' '||*first=='\t'))++first;
    if(first!=end&&*first=='+')++first;
    const auto parsed=std::from_chars(first,end,db);
    if(parsed.ec!=std::errc{}||!std::isfinite(db))return false;
    auto tail=parsed.ptr;while(tail!=end&&(*tail==' '||*tail=='\t'))++tail;
    if(end-tail>=2&&(tail[0]=='d'||tail[0]=='D')&&(tail[1]=='b'||tail[1]=='B'))tail+=2;
    while(tail!=end&&(*tail==' '||*tail=='\t'))++tail;
    return tail==end;
}
std::wstring Ansi(const char* s){if(!s)return{};int n=MultiByteToWideChar(CP_ACP,0,s,-1,nullptr,0);if(n<=0)return{};std::wstring v(n,0);MultiByteToWideChar(CP_ACP,0,s,-1,v.data(),n);v.pop_back();return v;}
}
extern "C" winamp::InModule* __cdecl winampGetInModule2(){static bool ready=[](){static char description[]="Independent Ogg Input 1.2.0 (Vorbis/Opus/FLAC/Speex, x86)";static char extensions[]="ogg;oga;spx\0Ogg Audio (*.ogg;*.oga;*.spx)\0";
    module.version=winamp::kInputVersion;module.description=description;module.FileExtensions=extensions;module.is_seekable=1;module.UsesOutputPlug=1;
    module.Init=Init;module.Quit=Quit;module.Config=Config;module.About=About;module.GetFileInfo=GetFileInfo;module.InfoBox=InfoBox;module.IsOurFile=IsOurFile;module.Play=Play;module.Stop=Stop;module.Pause=Pause;module.UnPause=Unpause;module.IsPaused=Paused;module.GetLength=Length;module.GetOutputTime=Time;module.SetOutputTime=Seek;module.SetVolume=Volume;module.SetPan=Pan;module.EQSet=Eq;return true;}();(void)ready;return &module;}
extern "C" int __cdecl winampGetExtendedFileInfoW(const wchar_t* path,const char* key,wchar_t* result,int capacity){if(!result||capacity<1||!key)return 0;result[0]=0;try{if((!path||!*path)&&!strcmp(key,"type")){Copy(result,capacity,L"0");return 1;}auto i=GetInfo(path);if(i.codec==Codec::Unknown)return 0;std::wstring value;if(!Metadata(i,key,value))return 0;Copy(result,capacity,value);return 1;}catch(...){return 0;}}
extern "C" int __cdecl winampGetExtendedFileInfo(const char* path,const char* key,char* result,int capacity){if(!result||capacity<1)return 0;result[0]=0;try{auto wide=Ansi(path);wchar_t value[32768]{};if(!winampGetExtendedFileInfoW(wide.c_str(),key,value,32768))return 0;int n=WideCharToMultiByte(CP_ACP,0,value,-1,nullptr,0,nullptr,nullptr);std::string text(n,0);WideCharToMultiByte(CP_ACP,0,value,-1,text.data(),n,nullptr,nullptr);strncpy_s(result,capacity,text.c_str(),_TRUNCATE);return 1;}catch(...){return 0;}}
extern "C" int __cdecl winampSetExtendedFileInfoW(const wchar_t* path,const char* key,const wchar_t* value){if(!path||!*path||!key||!value||IsHttp(path))return 0;try{auto info=GetInfo(path);if(info.codec!=Codec::Vorbis&&info.codec!=Codec::Opus)return 0;std::string name(key);if(name=="year")name="date";if(name=="track")name="tracknumber";if(name=="disc")name="discnumber";for(char& c:name){if(c<32||c>125||c=='=')return 0;c=static_cast<char>(tolower(static_cast<unsigned char>(c)));}if(name.empty()||name.size()>128||wcslen(value)>65536)return 0;
    std::wstring stored_value=value;if(name=="rating"&&*value){wchar_t* end=nullptr;long rating=wcstol(value,&end,10);if(!end||*end||rating<0||rating>5)return 0;stored_value=std::to_wstring(rating*20);}
    std::string obsolete_gain;
    if(info.codec==Codec::Opus&&(name=="replaygain_track_gain"||name=="replaygain_album_gain")){
        obsolete_gain=name;name=name=="replaygain_album_gain"?"r128_album_gain":"r128_track_gain";
        if(*value){double db=0;if(!ParseGain(value,db))return 0;
            const double q8=db*256.-info.current.header_gain_q8-5*256.;
            if(!std::isfinite(q8)||q8<-32768||q8>32767)return 0;
            stored_value=std::to_wstring(static_cast<int>(std::lround(q8)));
        }
    }
    for(const auto* technical:{"length","bitrate","channels","samplerate","type","lossless","family","formatinformation","mime","rateable","serial","gain"})if(name==technical)return 0;
    std::lock_guard<std::mutex> guard(lock);if(staged_path!=path){staged_path=path;staged_tags.clear();}
    if(!obsolete_gain.empty())staged_tags[obsolete_gain]=L"";
    staged_tags[name]=std::move(stored_value);return 1;}catch(...){return 0;}}
extern "C" int __cdecl winampSetExtendedFileInfo(const char* path,const char* key,const char* value){try{auto p=Ansi(path),v=Ansi(value);return winampSetExtendedFileInfoW(p.c_str(),key,v.c_str());}catch(...){return 0;}}
extern "C" int __cdecl winampWriteExtendedFileInfo(){try{std::wstring path;Tags tags;{std::lock_guard<std::mutex> guard(lock);path=staged_path;tags=staged_tags;}if(path.empty())return 0;std::wstring error;if(!WriteTags(path.c_str(),-1,tags,error))return 0;{std::lock_guard<std::mutex> guard(lock);staged_path.clear();staged_tags.clear();cache={};}return 1;}catch(...){return 0;}}
extern "C" int __cdecl winampUseUnifiedFileInfoDlg(const wchar_t* path){try{return path&&*path&&!IsHttp(path)&&GetSettings().unified_info&&GetInfo(path).codec!=Codec::Unknown;}catch(...){return 0;}}
extern "C" HWND __cdecl winampAddUnifiedFileInfoPane(int index,const wchar_t* path,HWND parent,wchar_t* name,int capacity){if(index||!path||!parent||capacity<1)return nullptr;try{Copy(name,capacity,Localized(L"Ogg audio"));return UnifiedPane(module.hDllInstance,parent,GetInfo(path));}catch(...){return nullptr;}}
