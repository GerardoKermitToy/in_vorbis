#include "codec_fixtures.hpp"
#include "stream.hpp"
#include "registry_sandbox.hpp"
#include "winamp_input.hpp"
#include <atomic>
#include <array>
#include <cmath>
#include <iostream>
namespace{
std::atomic<long long> frames{0};std::atomic<int> flushes{0};int channels=0,bits=0,opens=0,closes=0;
int __cdecl Open(int rate,int ch,int depth,int,int){Check(rate>=8000&&rate<=192000,"output rate");channels=ch;bits=depth;++opens;return 0;}
void __cdecl Close(){++closes;}
int __cdecl Write(char* p,int n){Check(p&&n>0&&n<=8192&&n%(channels*bits/8)==0,"write bounds");frames+=n/(channels*bits/8);return 0;}
int __cdecl CanWrite(){return 8192;}int __cdecl Playing(){return 0;}int __cdecl Pause(int n){return n;}void __cdecl Flush(int){++flushes;}int __cdecl Time(){return 0;}
void CheckLocalSeeks(winamp::InModule* in,const std::filesystem::path& path){
    int duration=-1;in->GetFileInfo(path.c_str(),nullptr,&duration);
    std::cout<<"DLL local duration before playback: "<<duration<<" ms\n";
    Check(duration>0,"DLL reports local duration before playback");
    Check(!in->Play(path.c_str()),"DLL local seek playback");in->Pause();
    Check(in->is_seekable&&in->GetLength()==duration,"DLL enables seek bar at startup");
    for(int position:{duration*3/4,duration/4,duration,0}){
        const int before=flushes;in->SetOutputTime(position);const auto end=GetTickCount64()+10000;
        while((flushes==before||in->GetOutputTime()!=position)&&GetTickCount64()<end)Sleep(1);
        Check(flushes==before+1&&in->GetOutputTime()==position,"DLL forward/backward paused seek flushes requested time");
        Check(in->GetLength()==duration,"DLL duration stable across seeks");
    }
    const auto before=frames.load();in->UnPause();const auto end=GetTickCount64()+10000;
    while(frames==before&&GetTickCount64()<end)Sleep(1);
    in->Stop();Check(frames>before,"DLL resumes PCM after seeking");
}
}
int wmain(int argc,wchar_t** argv){RegistrySandbox registry;HMODULE dll=nullptr;winamp::InModule* in=nullptr;try{
    Check(argc==3||argc==4,"usage: plugin_tests DLL DIRECTORY [RADIO_URL|LOCAL_FILE]");auto dir=std::filesystem::path(argv[2]);std::filesystem::create_directories(dir);auto vorbis=dir/L"file.ogg",opus=dir/L"file.oga";MakeVorbis(vorbis,2,44100,12001,4001);MakeOpus(opus,6,12001,4002);
    dll=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);Check(dll,"load DLL");auto get=reinterpret_cast<winamp::InModule*(__cdecl*)()>(GetProcAddress(dll,"winampGetInModule2"));Check(get,"plugin entry");in=get();winamp::OutModule out{};out.Open=Open;out.Close=Close;out.Write=Write;out.CanWrite=CanWrite;out.IsPlaying=Playing;out.Pause=Pause;out.Flush=Flush;out.GetOutputTime=Time;out.GetWrittenTime=Time;in->outMod=&out;in->hDllInstance=dll;in->Init();
    for(auto path:{vorbis,opus}){Check(in->IsOurFile(path.c_str()),"Ogg codec claim");frames=0;Check(!in->Play(path.c_str()),"play codec through DLL");auto end=GetTickCount64()+10000;while(frames<12001&&GetTickCount64()<end)Sleep(1);in->Stop();Check(frames==12001,"DLL output sample count");}
    Check(in->IsOurFile(L"http://icecast.radiosega.net:8006/rs-opus.ogg"),"claim RadioSEGA without network suffix");
    using OpenRead=intptr_t(__cdecl*)(const wchar_t*,int*,int*,int*,int*);using Read=size_t(__cdecl*)(intptr_t,char*,size_t,int*);using CloseRead=void(__cdecl*)(intptr_t);
    auto open=reinterpret_cast<OpenRead>(GetProcAddress(dll,"winampGetExtendedRead_openW"));auto read=reinterpret_cast<Read>(GetProcAddress(dll,"winampGetExtendedRead_getData"));auto close=reinterpret_cast<CloseRead>(GetProcAddress(dll,"winampGetExtendedRead_close"));Check(open&&read&&close,"extended read exports");int size=0,depth=16,ch=2,rate=0;auto h=open(vorbis.c_str(),&size,&depth,&ch,&rate);Check(h,"extended reader open");char buffer[4096]{};size_t count=0,n=0;while((n=read(h,buffer,sizeof(buffer),nullptr)))count+=n;close(h);Check(count==12001*4,"extended reader exact output");
    auto open_float=reinterpret_cast<OpenRead>(GetProcAddress(dll,"winampGetExtendedRead_openW_float"));
    using SeekRead=int(__cdecl*)(intptr_t,int);auto seek=reinterpret_cast<SeekRead>(GetProcAddress(dll,"winampGetExtendedRead_setTime"));
    Check(open_float&&seek,"float and seek exports");depth=0;ch=2;rate=0;h=open_float(vorbis.c_str(),&size,&depth,&ch,&rate);
    Check(h&&depth==32&&ch==2&&rate==44100,"float reader contract");std::array<float,1024> floating{};count=0;
    while((n=read(h,reinterpret_cast<char*>(floating.data()),sizeof(floating),nullptr))){Check(n%8==0,"float frame alignment");for(size_t i=0;i<n/4;++i)Check(std::isfinite(floating[i])&&std::abs(floating[i])<2.f,"finite float output");count+=n;}
    Check(count==12001*8,"float reader exact output");Check(seek(h,100),"extended seek");count=0;
    while((n=read(h,reinterpret_cast<char*>(floating.data()),sizeof(floating),nullptr)))count+=n;
    Check(count==(12001-4410)*8,"extended seek exact remaining frames");Check(seek(h,0),"extended rewind");int kill=1;
    Check(read(h,reinterpret_cast<char*>(floating.data()),sizeof(floating),&kill)==0,"extended kill switch");close(h);
    using GetTag=int(__cdecl*)(const wchar_t*,const char*,wchar_t*,int);
    using SetTag=int(__cdecl*)(const wchar_t*,const char*,const wchar_t*);using WriteTags=int(__cdecl*)();
    auto get_tag=reinterpret_cast<GetTag>(GetProcAddress(dll,"winampGetExtendedFileInfoW"));
    auto set_tag=reinterpret_cast<SetTag>(GetProcAddress(dll,"winampSetExtendedFileInfoW"));
    auto write_tags=reinterpret_cast<WriteTags>(GetProcAddress(dll,"winampWriteExtendedFileInfo"));Check(get_tag&&set_tag&&write_tags,"tag exports");
    Check(set_tag(vorbis.c_str(),"title",L"DLL Unicode title — テスト")&&set_tag(vorbis.c_str(),"rating",L"4"),"stage metadata");Check(write_tags(),"commit staged metadata");
    wchar_t tag[256]{};Check(get_tag(vorbis.c_str(),"title",tag,256)&&std::wstring(tag)==L"DLL Unicode title — テスト","metadata title roundtrip");
    Check(get_tag(vorbis.c_str(),"rating",tag,256)&&std::wstring(tag)==L"4","metadata rating mapping");
    Check(set_tag(vorbis.c_str(),"title",L"")&&write_tags(),"delete metadata field");
    Check(get_tag(vorbis.c_str(),"artist",tag,256)&&std::wstring(tag)==L"Independent tests","unrelated metadata preserved");
    Check(set_tag(opus.c_str(),"replaygain_track_gain",L"-2.25 dB")&&write_tags(),"Opus ReplayGain setter");
    Check(get_tag(opus.c_str(),"r128_track_gain",tag,256)&&std::wstring(tag)==L"-1856","Opus R128 gain conversion");
    Check(get_tag(opus.c_str(),"replaygain_track_gain",tag,256)&&std::wstring(tag)==L"-2.25","Opus ReplayGain getter conversion");
    Check(!set_tag(opus.c_str(),"replaygain_track_gain",L"not a gain"),"reject malformed ReplayGain");
    Check(set_tag(opus.c_str(),"replaygain_track_gain",L"")&&write_tags(),"Opus ReplayGain deletion");
    Check(!get_tag(opus.c_str(),"r128_track_gain",tag,256),"Opus gain tombstone");
#ifdef OGG_HAVE_SPEEX
    auto speex=dir/L"seek.spx";MakeSpeex(speex,32000,96001,4003,50);CheckLocalSeeks(in,speex);
#endif
    if(argc==4&&!ogg_input::IsHttp(argv[3]))CheckLocalSeeks(in,argv[3]);
    if(argc==4&&ogg_input::IsHttp(argv[3])){frames=0;Check(in->IsOurFile(argv[3]),"claim live radio URL");Check(!in->Play(argv[3]),"play live radio through DLL");auto deadline=GetTickCount64()+20000;while(frames<11520&&GetTickCount64()<deadline)Sleep(1);Check(!in->is_seekable,"live DLL seeking disabled");in->Stop();Check(frames>=11520,"live radio PCM output");std::cout<<"Live radio decoded through in_vorbis.dll: "<<frames<<" frames.\n";}
    Check(opens==closes,"output lifecycle");in->Quit();in=nullptr;FreeLibrary(dll);dll=nullptr;std::cout<<"DLL integration and extended read passed.\n";return 0;
}catch(const std::exception&e){if(in)in->Stop();if(dll)FreeLibrary(dll);std::cerr<<e.what()<<'\n';return 1;}}
