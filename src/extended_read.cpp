// SPDX-License-Identifier: GPL-3.0-only
#include "audio.hpp"
#include <windows.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <memory>
namespace {
struct Extended{ogg_input::Audio audio;std::unique_ptr<ogg_input::Pcm> pcm;int frame_bytes=0;bool floating=false;};
}
extern "C" intptr_t __cdecl winampGetExtendedRead_openW(const wchar_t* path,int* size,int* bits,int* channels,int* rate){
    if(!path||!size||!bits||!channels||!rate)return 0;try{auto reader=std::make_unique<Extended>();ogg_input::Settings s;s.bits=*bits==8||*bits==16||*bits==24||*bits==32?*bits:16;if(*channels==1)s.channels=ogg_input::ChannelMode::Mono;else if(*channels==2)s.channels=ogg_input::ChannelMode::Stereo;else if(*channels==4)s.channels=ogg_input::ChannelMode::Quad;s.rate=*rate>=8000&&*rate<=192000?*rate:0;s.dither=s.bits!=32;
        std::wstring error;if(!reader->audio.Open(path,s,false,error))return 0;reader->pcm=std::make_unique<ogg_input::Pcm>(reader->audio,s);if(!reader->pcm->Ready())return 0;
        *bits=s.bits;*channels=reader->audio.Channels();*rate=reader->audio.Rate();reader->frame_bytes=*channels*(*bits/8);
        auto ms=reader->audio.Info().duration_ms;auto bytes=ms<0?-1:static_cast<std::int64_t>(ms)*(*rate)*reader->frame_bytes/1000;*size=bytes<0||bytes>INT_MAX?-1:static_cast<int>(bytes);return reinterpret_cast<intptr_t>(reader.release());
    }catch(...){return 0;}}
extern "C" intptr_t __cdecl winampGetExtendedRead_openW_float(const wchar_t* path,int* size,int* bits,int* channels,int* rate){
    if(!bits)return 0;*bits=32;auto handle=winampGetExtendedRead_openW(path,size,bits,channels,rate);if(handle)reinterpret_cast<Extended*>(handle)->floating=true;return handle;
}
extern "C" intptr_t __cdecl winampGetExtendedRead_open(const char* path,int* size,int* bits,int* channels,int* rate){if(!path)return 0;try{int n=MultiByteToWideChar(CP_ACP,0,path,-1,nullptr,0);if(n<=0)return 0;std::wstring p(n,0);MultiByteToWideChar(CP_ACP,0,path,-1,p.data(),n);return winampGetExtendedRead_openW(p.c_str(),size,bits,channels,rate);}catch(...){return 0;}}
extern "C" size_t __cdecl winampGetExtendedRead_getData(intptr_t handle,char* buffer,size_t capacity,int* kill){if(!handle||!buffer)return 0;try{auto& r=*reinterpret_cast<Extended*>(handle);size_t used=0;
    while(capacity-used>=static_cast<size_t>(r.frame_bytes)&&(!kill||!*kill)){int frames=static_cast<int>(std::min<size_t>(ogg_input::BlockFrames,(capacity-used)/r.frame_bytes));int n=r.floating?r.pcm->ReadFloat(reinterpret_cast<float*>(buffer+used),frames):r.pcm->Read(buffer+used,frames);if(n<=0)break;used+=static_cast<size_t>(n)*r.frame_bytes;}return used;
}catch(...){return 0;}}
extern "C" int __cdecl winampGetExtendedRead_setTime(intptr_t handle,int ms){if(!handle)return 0;try{auto& r=*reinterpret_cast<Extended*>(handle);if(!r.audio.Seek(ms))return 0;r.pcm->Reset(ms);return 1;}catch(...){return 0;}}
extern "C" void __cdecl winampGetExtendedRead_close(intptr_t handle){delete reinterpret_cast<Extended*>(handle);}
