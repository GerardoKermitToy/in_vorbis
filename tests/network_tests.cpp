#include <winsock2.h>
#include <ws2tcpip.h>
#include "audio.hpp"
#include "codec_fixtures.hpp"
#include "registry_sandbox.hpp"
#include "winamp_input.hpp"
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <vector>
#include <cstdlib>
#include <cstring>
using namespace ogg_input;
namespace {
std::vector<char> Bytes(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
class Server{
    SOCKET socket_=INVALID_SOCKET;unsigned short port_=0;std::thread worker_;std::atomic<bool> stop_{false};std::vector<char> opus_,vorbis_;
    static bool Send(SOCKET s,const char* p,int n){while(n){int made=send(s,p,n,0);if(made<=0)return false;p+=made;n-=made;}return true;}
    void Run(){while(!stop_){fd_set ready;FD_ZERO(&ready);FD_SET(socket_,&ready);timeval timeout{0,100000};if(select(0,&ready,nullptr,nullptr,&timeout)<=0)continue;SOCKET c=accept(socket_,nullptr,nullptr);if(c==INVALID_SOCKET)continue;DWORD ms=1000;setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&ms),4);setsockopt(c,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<char*>(&ms),4);
        char buffer[1024]{};std::string request;while(request.size()<8192&&request.find("\r\n\r\n")==std::string::npos){int n=recv(c,buffer,sizeof(buffer),0);if(n<=0)break;request.append(buffer,n);}
        bool icy=request.find("GET /icy ")==0,chunked=request.find("GET /chunked ")==0,vorbis=request.find("GET /vorbis.ogg ")==0,slow=request.find("GET /slow ")==0;
        if(request.find("GET /file ")==0||request.find("GET /file-vorbis ")==0){
            const auto& body=request.find("GET /file-vorbis ")==0?vorbis_:opus_;std::size_t start=0;auto range=request.find("\r\nRange: bytes=");bool partial=range!=std::string::npos;if(partial)start=static_cast<std::size_t>(std::strtoull(request.c_str()+range+15,nullptr,10));
            if(start>=body.size()){constexpr char response[]="HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";Send(c,response,sizeof(response)-1);}
            else{std::string h=partial?"HTTP/1.1 206 Partial Content\r\n":"HTTP/1.1 200 OK\r\n";h+="Accept-Ranges: bytes\r\nContent-Type: audio/ogg\r\nConnection: close\r\nContent-Length: "+std::to_string(body.size()-start)+"\r\n";if(partial)h+="Content-Range: bytes "+std::to_string(start)+"-"+std::to_string(body.size()-1)+"/"+std::to_string(body.size())+"\r\n";h+="\r\n";Send(c,h.data(),static_cast<int>(h.size()));Send(c,body.data()+start,static_cast<int>(body.size()-start));}
        }
        else if(request.find("GET /redirect ")==0){std::string h="HTTP/1.1 302 Found\r\nLocation: /stream.ogg\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";Send(c,h.data(),static_cast<int>(h.size()));}
        else{std::vector<char> body=vorbis?vorbis_:opus_;if(icy){std::vector<char> replaced;std::string metadata="StreamTitle='Loopback title';";metadata.resize(32,'\0');for(std::size_t at=0;at<body.size();){auto n=std::min<std::size_t>(128,body.size()-at);replaced.insert(replaced.end(),body.begin()+at,body.begin()+at+n);at+=n;if(n==128){replaced.push_back(2);replaced.insert(replaced.end(),metadata.begin(),metadata.end());}}body=std::move(replaced);}
            std::string h="HTTP/1.1 200 OK\r\nContent-Type: application/ogg\r\nicy-name: Loopback Radio\r\nicy-br: 128\r\nConnection: close\r\n";if(icy)h+="icy-metaint: 128\r\n";if(chunked)h+="Transfer-Encoding: chunked\r\n";else if(!slow)h+="Content-Length: "+std::to_string(body.size())+"\r\n";h+="\r\n";Send(c,h.data(),static_cast<int>(h.size()));
            if(slow){while(!stop_)std::this_thread::sleep_for(std::chrono::milliseconds(5));}
            else if(chunked){for(std::size_t at=0;at<body.size();){int n=static_cast<int>(std::min<std::size_t>(47,body.size()-at));char hex[32]{};sprintf_s(hex,"%x\r\n",n);if(!Send(c,hex,static_cast<int>(strlen(hex)))||!Send(c,body.data()+at,n)||!Send(c,"\r\n",2))break;at+=n;}Send(c,"0\r\n\r\n",5);}
            else Send(c,body.data(),static_cast<int>(body.size()));
        }closesocket(c);
    }}
public:
    Server(std::vector<char> op,std::vector<char> vo):opus_(std::move(op)),vorbis_(std::move(vo)){WSADATA d{};Check(!WSAStartup(MAKEWORD(2,2),&d),"WSA init");socket_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);Check(socket_!=INVALID_SOCKET,"server socket");sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);Check(!bind(socket_,reinterpret_cast<sockaddr*>(&a),sizeof(a)),"server bind");int n=sizeof(a);Check(!getsockname(socket_,reinterpret_cast<sockaddr*>(&a),&n)&&!listen(socket_,5),"server listen");port_=ntohs(a.sin_port);worker_=std::thread([this]{Run();});}
    ~Server(){stop_=true;closesocket(socket_);if(worker_.joinable())worker_.join();WSACleanup();}
    std::wstring Url(const wchar_t* path){return L"http://127.0.0.1:"+std::to_wstring(port_)+path;}
};
#ifndef OGG_STREAM_ONLY
std::atomic<long long> plugin_frames{0};
int output_opens=0,output_closes=0;
int __cdecl OutputOpen(int rate,int channels,int bits,int,int){Check(rate==48000&&channels==2&&bits==16,"radio output format");++output_opens;return 0;}
void __cdecl OutputClose(){++output_closes;}
int __cdecl OutputWrite(char* data,int bytes){Check(data&&bytes>0&&bytes<=8192&&bytes%4==0,"radio output bounds");plugin_frames+=bytes/4;return 0;}
int __cdecl OutputCanWrite(){return 8192;}
int __cdecl OutputPlaying(){return 0;}
int __cdecl OutputPause(int n){return n;}
void __cdecl OutputFlush(int){}
int __cdecl OutputTime(){return 0;}
void CheckReconnect(Server& server,const wchar_t* dll_path){
    RegistrySandbox registry;
    HKEY key=nullptr;DWORD disposition=0;
    Check(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\WinampOgg\\Input",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,&disposition)==ERROR_SUCCESS,"create reconnect settings");
    DWORD attempts=1,delay=250;
    Check(RegSetValueExW(key,L"ReconnectAttempts",0,REG_DWORD,reinterpret_cast<BYTE*>(&attempts),4)==ERROR_SUCCESS&&
          RegSetValueExW(key,L"ReconnectDelayMs",0,REG_DWORD,reinterpret_cast<BYTE*>(&delay),4)==ERROR_SUCCESS,"set reconnect policy");RegCloseKey(key);
    HMODULE dll=LoadLibraryExW(dll_path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);Check(dll,"load network DLL");
    winamp::InModule* module=nullptr;
    try{
        auto get=reinterpret_cast<winamp::InModule*(__cdecl*)()>(GetProcAddress(dll,"winampGetInModule2"));Check(get,"network input entry");module=get();
        winamp::OutModule output{};output.Open=OutputOpen;output.Close=OutputClose;output.Write=OutputWrite;output.CanWrite=OutputCanWrite;output.IsPlaying=OutputPlaying;output.Pause=OutputPause;output.Flush=OutputFlush;output.GetOutputTime=OutputTime;output.GetWrittenTime=OutputTime;
        module->hDllInstance=dll;module->outMod=&output;module->Init();plugin_frames=0;
        Check(!module->Play(server.Url(L"/stream.ogg").c_str()),"DLL radio connection");
        auto deadline=GetTickCount64()+10000;while(plugin_frames<48002&&GetTickCount64()<deadline)Sleep(1);
        module->Stop();Check(plugin_frames>=48002,"DLL reconnect decodes a second connection");
        Check(output_opens==1&&output_closes==1,"reconnect retains output lifecycle");
        module->Quit();module=nullptr;FreeLibrary(dll);
    }catch(...){if(module){module->Stop();module->Quit();}FreeLibrary(dll);throw;}
}
#endif
}
#ifdef OGG_STREAM_ONLY
int wmain(int argc,wchar_t**argv){try{Check(argc==2,"usage stream_tests DIRECTORY");std::filesystem::path dir(argv[1]);std::filesystem::create_directories(dir);auto op=dir/L"source.opus",vo=dir/L"source.ogg";MakeOpus(op,2,24001,9101);MakeVorbis(vo,2,44100,24001,9102);auto expected=Bytes(op);Server server(expected,Bytes(vo));
    for(auto suffix:{L"/stream.ogg",L"/redirect",L"/chunked",L"/icy"}){Settings s;std::wstring error;ByteStream stream;Check(stream.Open(server.Url(suffix).c_str(),s,error),ToUtf8(error).c_str());Check(Probe(stream)==Codec::Opus,"probe on buffered HTTP");stream.FinishProbe();std::vector<char> body;char data[257];int n;while((n=stream.Read(data,sizeof(data)))>0)body.insert(body.end(),data,data+n);Check(n==0&&body==expected,"HTTP body after redirects, chunked and ICY stripping");}
    {Settings s;s.http_seek=true;ByteStream stream;std::wstring error;Check(stream.Open(server.Url(L"/file").c_str(),s,error),"range-capable HTTP open");Check(stream.Seekable()&&!stream.Radio(),"finite range stream properties");Check(stream.Seek(123,SEEK_SET),"HTTP byte seek");char data[177]{};Check(stream.Read(data,sizeof(data))==sizeof(data)&&std::equal(data,data+sizeof(data),expected.data()+123),"HTTP exact range bytes");Check(stream.Seek(0,SEEK_END)&&stream.Read(data,sizeof(data))==0,"HTTP seek to EOF");Check(stream.Seek(0,SEEK_SET)&&Probe(stream)==Codec::Opus,"HTTP rewind probe");}
    Settings s;s.prebuffer_kb=0;ByteStream stalled;std::wstring error;Check(stalled.Open(server.Url(L"/slow").c_str(),s,error),"stalled HTTP open");auto future=std::async(std::launch::async,[&]{char data[10];return stalled.Read(data,sizeof(data));});std::this_thread::sleep_for(std::chrono::milliseconds(50));stalled.Cancel();Check(future.wait_for(std::chrono::seconds(2))==std::future_status::ready&&future.get()<0,"cancel pending network read");std::cout<<"HTTP byte transport and cancellation passed.\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
#else
int wmain(int argc,wchar_t** argv){try{Check(argc==2||argc==3,"usage network_tests DIRECTORY [DLL]");std::filesystem::path dir(argv[1]);std::filesystem::create_directories(dir);auto op=dir/L"radio.opus",vo=dir/L"radio-vorbis.ogg";MakeOpus(op,2,24001,8001);MakeVorbis(vo,2,44100,24001,8002);Server server(Bytes(op),Bytes(vo));
    for(auto path:{L"/stream.ogg",L"/redirect",L"/icy",L"/chunked",L"/vorbis.ogg"}){Audio a;Settings s;std::wstring error;Check(a.Open(server.Url(path).c_str(),s,false,error),ToUtf8(error).c_str());Check(a.Info().radio&&!a.Info().seekable&&a.Channels()==2,"radio properties");Pcm pcm(a,s);std::array<short,BlockFrames*2> data{};int total=0;for(;;){int n=pcm.Read(data.data(),BlockFrames);Check(n>=0,"network decode");if(!n)break;total+=n;}Check(total==24001,"network audio length");Check(a.Info().station.name==L"Loopback Radio","station headers");}
    {Audio a;Settings s;s.http_seek=true;std::wstring error;Check(a.Open(server.Url(L"/file-vorbis").c_str(),s,false,error),ToUtf8(error).c_str());Check(a.Info().seekable&&!a.Info().radio,"seekable HTTP file flags");Check(a.Seek(100),"HTTP file time seek");Pcm pcm(a,s);std::array<short,BlockFrames*2> data{};int frames=0;for(;;){int n=pcm.Read(data.data(),BlockFrames);Check(n>=0,"range HTTP decode");if(!n)break;frames+=n;}Check(std::abs(frames-(24001-4410))<=1,"HTTP seek remaining frames");}
    auto record_dir=dir/L"recordings";std::filesystem::create_directories(record_dir);std::size_t before=0;for(auto& e:std::filesystem::directory_iterator(record_dir)){(void)e;++before;}
    {Audio a;Settings s;s.save_streams=true;s.save_directory=record_dir.wstring();std::wstring error;Check(a.Open(server.Url(L"/icy").c_str(),s,false,error),"recording connection");std::array<float,BlockFrames*2> data{};while(a.Read(data.data(),BlockFrames)>0){}}
    std::size_t after=0;bool matching=false;for(auto& e:std::filesystem::directory_iterator(record_dir)){++after;if(Bytes(e.path())==Bytes(op))matching=true;}Check(after==before+1&&matching,"recording preserves demetadata Ogg bytes");
    if(argc==3)CheckReconnect(server,argv[2]);
    Settings s;s.prebuffer_kb=0;ByteStream stalled;std::wstring error;Check(stalled.Open(server.Url(L"/slow").c_str(),s,error),"open stalled server");auto read=std::async(std::launch::async,[&]{char data[512];return stalled.Read(data,sizeof(data));});std::this_thread::sleep_for(std::chrono::milliseconds(50));stalled.Cancel();Check(read.wait_for(std::chrono::seconds(2))==std::future_status::ready&&read.get()<0,"cancel blocked network read");
    std::cout<<"HTTP Vorbis/Opus, redirects, chunked transfer, ICY, recording, cancellation passed.\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}

#endif
