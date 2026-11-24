// SPDX-License-Identifier: GPL-3.0-only
#include "stream.hpp"
#include "credentials.hpp"
#include <algorithm>
#include <climits>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <io.h>
#include <fcntl.h>
namespace ogg_input {
bool IsHttp(const wchar_t* p) noexcept { return p && (!_wcsnicmp(p,L"http://",7) || !_wcsnicmp(p,L"https://",8)); }
std::wstring RequestUrl(const wchar_t* p) {
    std::wstring s(p ? p : L"");
    for (const auto* hint : {L">.opus", L">.ogg", L">.oga"}) {
        const auto n = wcslen(hint); if (s.size() >= n && !_wcsicmp(s.c_str()+s.size()-n,hint)) { s.resize(s.size()-n); break; }
    }
    return s;
}
std::wstring Utf8(const char* text,int length) {
    if (!text || length == 0) return {};
    const int n=MultiByteToWideChar(CP_UTF8,0,text,length,nullptr,0); if(n<=0)return{};
    std::wstring out(n,L'\0'); MultiByteToWideChar(CP_UTF8,0,text,length,out.data(),n);
    if(length<0 && !out.empty())out.pop_back(); return out;
}
std::string ToUtf8(const std::wstring& text) {
    if(text.empty())return{}; int n=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string out(n,'\0'); WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),n,nullptr,nullptr); return out;
}
ByteStream::~ByteStream(){Cancel();if(recording_!=INVALID_HANDLE_VALUE)CloseHandle(recording_);if(file_)fclose(file_);if(connection_)WinHttpCloseHandle(connection_);if(session_)WinHttpCloseHandle(session_);}
void ByteStream::Cancel() noexcept {cancelled_=true;if(auto h=request_.exchange(nullptr))WinHttpCloseHandle(h);}
std::wstring ByteStream::Header(const wchar_t* name)const{
    DWORD bytes=0;auto h=request_.load();WinHttpQueryHeaders(h,WINHTTP_QUERY_CUSTOM,name,nullptr,&bytes,nullptr);
    if(GetLastError()!=ERROR_INSUFFICIENT_BUFFER||bytes>65536)return{};
    std::vector<wchar_t> v(bytes/2+1);if(!WinHttpQueryHeaders(h,WINHTTP_QUERY_CUSTOM,name,v.data(),&bytes,nullptr))return{};return v.data();
}
bool ByteStream::Open(const wchar_t* path,const Settings& raw,std::wstring& error){
    const auto s=Validate(raw);settings_=s;if(cancelled_){error=L"The connection was cancelled.";return false;} if(!path||!*path){error=L"No file or URL was supplied.";return false;}
    radio_=IsHttp(path);probe_limit_=s.probe_kb*1024;
    if(!radio_){
        auto open_file=[](const wchar_t* name)->FILE*{auto h=CreateFileW(name,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return nullptr;
            int fd=_open_osfhandle(reinterpret_cast<intptr_t>(h),_O_RDONLY|_O_BINARY);if(fd<0){CloseHandle(h);return nullptr;}auto* f=_fdopen(fd,"rb");if(!f)_close(fd);return f;};
        file_=open_file(path);
        if(!file_&&wcschr(path,L'|')){std::wstring fixed(path);std::replace(fixed.begin(),fixed.end(),L'|',L'｜');file_=open_file(fixed.c_str());}
        if(!file_){error=L"Cannot open the audio file for reading.";return false;}
        if(_fseeki64(file_,0,SEEK_END)|| (size_=_ftelli64(file_))<0 || _fseeki64(file_,0,SEEK_SET)){error=L"Cannot determine the file size.";return false;}
        source_size_=size_;
        // Some historical Ogg files wrap the bitstream in a RIFF/WAVE data chunk.
        unsigned char header[12]{};if(fread(header,1,12,file_)==12&&!memcmp(header,"RIFF",4)&&!memcmp(header+8,"WAVE",4)){
            const auto physical=size_;std::int64_t at=12;bool found=false;
            while(at<=physical-8&&at<64*1024*1024){if(_fseeki64(file_,at,SEEK_SET))break;unsigned char chunk[8]{};if(fread(chunk,1,8,file_)!=8)break;
                auto length=static_cast<std::uint32_t>(chunk[4])|(static_cast<std::uint32_t>(chunk[5])<<8)|(static_cast<std::uint32_t>(chunk[6])<<16)|(static_cast<std::uint32_t>(chunk[7])<<24);
                if(length>static_cast<std::uint64_t>(physical-at-8))break;
                if(!memcmp(chunk,"data",4)){base_offset_=at+8;size_=length;found=true;break;}at+=8+static_cast<std::int64_t>(length)+(length&1);
            }
            if(!found){error=L"The WAVE wrapper has no valid Ogg data chunk.";return false;}
        }
        if(_fseeki64(file_,base_offset_,SEEK_SET)){error=L"Cannot read the audio data.";return false;}
        if(s.buffer_small_files&&size_>0&&size_<=64*1024*1024){
            memory_.resize(static_cast<std::size_t>(size_));if(fread(memory_.data(),1,memory_.size(),file_)!=memory_.size()){error=L"Cannot buffer the audio file.";return false;}
            fclose(file_);file_=nullptr;
        }
        return true;
    }
    const auto url=RequestUrl(path); URL_COMPONENTS p{};p.dwStructSize=sizeof(p);
    p.dwHostNameLength=p.dwUrlPathLength=p.dwExtraInfoLength=p.dwUserNameLength=p.dwPasswordLength=DWORD(-1);
    if(!WinHttpCrackUrl(url.c_str(),0,0,&p)||p.dwUserNameLength||p.dwPasswordLength){error=L"Invalid HTTP URL. Credentials must not be embedded in a URL.";return false;}
    std::wstring host(p.lpszHostName,p.dwHostNameLength),resource(p.lpszUrlPath,p.dwUrlPathLength);
    if(resource.empty())resource=L"/";if(p.dwExtraInfoLength)resource.append(p.lpszExtraInfo,p.dwExtraInfoLength);
    if(auto hash=resource.find(L'#');hash!=std::wstring::npos)resource.resize(hash);
    resource_=resource;request_flags_=p.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0;
    bool use_proxy=s.proxy_mode==2||(s.proxy_mode==1&&p.nPort==80);
    session_=WinHttpOpen(L"OggAudioInput/1.0",!use_proxy?WINHTTP_ACCESS_TYPE_NO_PROXY:s.proxy_url.empty()?WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY:WINHTTP_ACCESS_TYPE_NAMED_PROXY,!use_proxy||s.proxy_url.empty()?WINHTTP_NO_PROXY_NAME:s.proxy_url.c_str(),WINHTTP_NO_PROXY_BYPASS,0);
    if(!session_||!WinHttpSetTimeouts(session_,s.timeout_ms,s.timeout_ms,s.timeout_ms,s.timeout_ms)){error=L"Cannot initialize Windows HTTP.";return false;}
    connection_=WinHttpConnect(session_,host.c_str(),p.nPort,0);if(!connection_){error=L"Cannot connect to the HTTP server.";return false;}
    auto request=WinHttpOpenRequest(connection_,L"GET",resource.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,p.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);
    request_=request;if(cancelled_){if(auto h=request_.exchange(nullptr))WinHttpCloseHandle(h);error=L"The connection was cancelled.";return false;}if(!request){error=L"Cannot create the HTTP request.";return false;}
    DWORD auth=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH,redirect=WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP,redirects=5;
    if(!WinHttpSetOption(request,WINHTTP_OPTION_AUTOLOGON_POLICY,&auth,sizeof(auth))||!WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect))){error=L"Cannot apply safe HTTP policy.";return false;}
    WinHttpSetOption(request,WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS,&redirects,sizeof(redirects));
    constexpr wchar_t headers[]=L"Icy-MetaData: 1\r\nAccept: audio/ogg, application/ogg, audio/opus, */*\r\n";
    if(!WinHttpSendRequest(request,headers,DWORD(-1),WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request,nullptr)){
        error=L"HTTP connection failed (Windows error "+std::to_wstring(GetLastError())+L").";return false;}
    DWORD status=0,n=sizeof(status);if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&n,nullptr)){error=L"Cannot read HTTP status.";return false;}
    for(int attempt=0;s.interactive_auth&&(status==401||status==407)&&attempt<3;++attempt){
        const bool proxy=status==407;DWORD supported=0,first=0,target=0;
        if(!WinHttpQueryAuthSchemes(request,&supported,&first,&target))break;
        DWORD scheme=(supported&WINHTTP_AUTH_SCHEME_DIGEST)?WINHTTP_AUTH_SCHEME_DIGEST:(supported&WINHTTP_AUTH_SCHEME_BASIC)?WINHTTP_AUTH_SCHEME_BASIC:0;
        if(!scheme)break;
        if(scheme==WINHTTP_AUTH_SCHEME_BASIC&&(proxy||p.nScheme!=INTERNET_SCHEME_HTTPS)&&MessageBoxW(nullptr,L"This server requests Basic authentication without end-to-end TLS protection. Continue only if you trust this connection.",L"Ogg Stream Authentication",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES)break;
        std::wstring user,password;if(!AskCredentials(nullptr,proxy,proxy?s.proxy_url:host,user,password))break;
        BOOL applied=WinHttpSetCredentials(request,proxy?WINHTTP_AUTH_TARGET_PROXY:WINHTTP_AUTH_TARGET_SERVER,scheme,user.c_str(),password.c_str(),nullptr);
        if(!password.empty())SecureZeroMemory(password.data(),password.size()*sizeof(wchar_t));
        if(!applied||!WinHttpSendRequest(request,headers,DWORD(-1),WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request,nullptr))break;
        n=sizeof(status);if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&n,nullptr))break;
    }
    if(status!=200){error=L"HTTP server returned status "+std::to_wstring(status)+L".";return false;}
    station_.name=Header(L"icy-name");station_.description=Header(L"icy-description");station_.genre=Header(L"icy-genre");station_.website=Header(L"icy-url");station_.content_type=Header(L"Content-Type");
    auto br=wcstoul(Header(L"icy-br").c_str(),nullptr,10);if(br<=100000)station_.bitrate=static_cast<int>(br)*1000;
    auto interval=wcstoul(Header(L"icy-metaint").c_str(),nullptr,10);if(interval>16*1024*1024){error=L"Invalid ICY metadata interval.";return false;}
    icy_interval_=icy_left_=static_cast<int>(interval);deadline_=GetTickCount64()+15000;
    auto length=Header(L"Content-Length");wchar_t* length_end=nullptr;
    if(!length.empty()){auto value=_wcstoui64(length.c_str(),&length_end,10);if(length_end&&!*length_end&&value<=INT64_MAX)size_=static_cast<std::int64_t>(value);}
    source_size_=size_;
    http_seekable_=s.http_seek&&size_>=0&&!icy_interval_&&!_wcsicmp(Header(L"Accept-Ranges").c_str(),L"bytes");
    network_buffer_.resize(static_cast<std::size_t>(s.stream_buffer_kb)*1024);
    if(s.prebuffer_kb>0){int wanted=s.prebuffer_kb*1024;while(network_used_<wanted){int got=NetworkRead(network_buffer_.data()+network_used_,wanted-network_used_);if(got<0){error=L"Cannot prebuffer the radio stream.";return false;}if(!got)break;network_used_+=got;}}
    return true;
}
int ByteStream::RawRead(void* dst,int n)noexcept{
    if(n<=0)return 0;if(cancelled_)return -1;
    if(file_){auto wanted=static_cast<std::size_t>(std::min<std::int64_t>(n,std::max<std::int64_t>(0,size_-position_)));auto count=fread(dst,1,wanted,file_);return !count&&ferror(file_)?-1:static_cast<int>(count);}
    if(!radio_){auto count=static_cast<int>(std::min<std::int64_t>(n,size_-position_));if(count>0)memcpy(dst,memory_.data()+position_,count);return count;}
    if(http_seekable_&&position_>=size_)return 0;
    if(network_at_>=network_used_){
        network_at_=network_used_=0;const int target=std::max(1,static_cast<int>(network_buffer_.size())*settings_.underrun_percent/100);
        do{int got=NetworkRead(network_buffer_.data()+network_used_,static_cast<int>(network_buffer_.size())-network_used_);if(got<0)return -1;if(!got)break;network_used_+=got;}while(network_used_<target);
        if(!network_used_)return 0;
    }
    int got=std::min(n,network_used_-network_at_);memcpy(dst,network_buffer_.data()+network_at_,got);network_at_+=got;return got;
}
int ByteStream::NetworkRead(void* dst,int n)noexcept{
    if(cancelled_||n<=0)return n<=0?0:-1;
    if(opening_&&(GetTickCount64()>deadline_||network_bytes_>=probe_limit_))return -1;
    DWORD got=0;auto h=request_.load();if(!h||!WinHttpReadData(h,dst,static_cast<DWORD>(opening_?std::min(n,probe_limit_-network_bytes_):n),&got))return -1;
    if(opening_)network_bytes_+=static_cast<int>(got);return static_cast<int>(got);
}
bool ByteStream::Exact(void* dst,int n)noexcept{auto p=static_cast<unsigned char*>(dst);while(n){int got=RawRead(p,n);if(got<=0)return false;p+=got;n-=got;}return true;}
int ByteStream::Read(void* dst,int bytes)noexcept{
    if(bytes<=0)return 0;if(cancelled_)return -1;
    if(replaying_){int n=static_cast<int>(std::min<std::size_t>(bytes,prefix_.size()-replay_));if(n){memcpy(dst,prefix_.data()+replay_,n);replay_+=n;position_+=n;return n;}replaying_=false;}
    int got;
    if(radio_&&icy_interval_){
        if(!icy_left_){unsigned char units=0;if(!Exact(&units,1))return -1;char metadata[4081]{};if(units&&!Exact(metadata,units*16))return -1;
            // ICY payload is bounded to 255*16 bytes and never reaches a codec.
            try{std::string m(metadata,units*16);auto at=m.find("StreamTitle='");if(at!=std::string::npos){auto end=m.find("';",at+13);if(end!=std::string::npos)station_.title=Utf8(m.data()+at+13,static_cast<int>(end-at-13));}}catch(...){return -1;}
            icy_left_=icy_interval_;}
        got=RawRead(dst,std::min(bytes,icy_left_));if(got>0)icy_left_-=got;
    }else got=RawRead(dst,bytes);
    if(got>0){
        if(opening_&&radio_&&!http_seekable_){try{if(prefix_.size()+got<=static_cast<std::size_t>(probe_limit_))prefix_.insert(prefix_.end(),static_cast<unsigned char*>(dst),static_cast<unsigned char*>(dst)+got);else return -1;}catch(...){return -1;}}
        position_+=got;if(!opening_)Record(dst,got);
    }return got;
}
bool ByteStream::Seek(std::int64_t offset,int origin)noexcept{
    if(cancelled_||(radio_&&!http_seekable_))return false;
    const auto base=origin==SEEK_SET?0:origin==SEEK_CUR?position_:origin==SEEK_END?size_:-1;
    if(base<0||offset<-base||offset>size_-base)return false;auto next=base+offset;
    if(http_seekable_){if(next==position_)return true;if(next==size_){position_=next;network_at_=network_used_=0;prefix_.clear();replaying_=false;return true;}return Range(next);}
    if(file_&&_fseeki64(file_,base_offset_+next,SEEK_SET))return false;position_=next;return true;
}
bool ByteStream::Range(std::int64_t offset)noexcept{
    try{
        if(cancelled_||!http_seekable_)return false;
        if(auto old=request_.exchange(nullptr))WinHttpCloseHandle(old);
        auto request=WinHttpOpenRequest(connection_,L"GET",resource_.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,request_flags_);request_=request;
        if(!request||cancelled_){if(auto h=request_.exchange(nullptr))WinHttpCloseHandle(h);return false;}
        DWORD policy=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH,redirect=WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        if(!WinHttpSetOption(request,WINHTTP_OPTION_AUTOLOGON_POLICY,&policy,sizeof(policy))||!WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect)))return false;
        auto headers=L"Icy-MetaData: 0\r\nRange: bytes="+std::to_wstring(offset)+L"-\r\n";
        if(!WinHttpSendRequest(request,headers.c_str(),DWORD(-1),WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(request,nullptr))return false;
        DWORD status=0,bytes=sizeof(status);if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&bytes,nullptr)||status!=206)return false;
        auto content=Header(L"Content-Range");unsigned long long first=0,last=0,total=0;wchar_t extra=0;
        if(swscanf_s(content.c_str(),L"bytes %llu-%llu/%llu %lc",&first,&last,&total,&extra,1)!=3||first!=static_cast<unsigned long long>(offset)||last<first||total!=static_cast<unsigned long long>(size_)||last>=total)return false;
        network_at_=network_used_=0;position_=offset;prefix_.clear();replaying_=false;return true;
    }catch(...){return false;}
}
void ByteStream::Record(const void* data,int size)noexcept{
    if(recording_==INVALID_HANDLE_VALUE||size<=0)return;DWORD wrote=0;
    if(!WriteFile(recording_,data,static_cast<DWORD>(size),&wrote,nullptr)||wrote!=static_cast<DWORD>(size)){CloseHandle(recording_);recording_=INVALID_HANDLE_VALUE;try{station_.recording_error=L"Recording stopped: the destination could not be written.";}catch(...){}}
}
void ByteStream::FinishProbe(){
    opening_=false;
    if(radio_&&!http_seekable_&&settings_.save_streams&&!settings_.save_directory.empty()){
        auto directory=std::filesystem::path(settings_.save_directory);DWORD attributes=GetFileAttributesW(directory.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)){
            SYSTEMTIME now{};GetLocalTime(&now);wchar_t name[128]{};swprintf_s(name,L"Ogg radio %04u-%02u-%02u %02u-%02u-%02u %llu.ogg",now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond,GetTickCount64());
            auto path=directory/name;recording_=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(recording_!=INVALID_HANDLE_VALUE){station_.recording_path=path.wstring();Record(prefix_.data(),static_cast<int>(prefix_.size()));}
            else station_.recording_error=L"Cannot create the stream recording (Windows error "+std::to_wstring(GetLastError())+L").";
        }else station_.recording_error=L"The configured recording directory is unavailable.";
    }
    if(!replaying_){prefix_.clear();prefix_.shrink_to_fit();}
}
std::int64_t ByteStream::Tell()const noexcept{return position_;}
bool ByteStream::RewindProbe()noexcept{
    if(Seekable())return Seek(0,SEEK_SET);if(!radio_||position_!=static_cast<std::int64_t>(prefix_.size()))return false;
    replay_=0;position_=0;replaying_=true;return true;
}
}
