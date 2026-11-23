// SPDX-License-Identifier: GPL-3.0-only
#include "backend.hpp"
#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <sstream>
namespace ogg_input {
const wchar_t* CodecName(Codec c)noexcept{switch(c){case Codec::Vorbis:return L"Ogg Vorbis";case Codec::Opus:return L"Ogg Opus";case Codec::Flac:return L"Ogg FLAC";case Codec::Speex:return L"Ogg Speex";default:return L"Unknown Ogg audio";}}
void AddTag(Tags& tags,const char* text,int n){
    if(!text||n<=0||n>65536||tags.size()>512)return;std::string s(text,n);if(s.find('\0')!=std::string::npos)return;auto at=s.find('=');if(at==std::string::npos||at<1||at>128)return;
    auto key=s.substr(0,at);for(auto& c:key){if(static_cast<unsigned char>(c)<32||static_cast<unsigned char>(c)>125)return;c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));}
    if(key=="metadata_block_picture"||key=="coverart")return;
    auto value=Utf8(s.data()+at+1,static_cast<int>(s.size()-at-1));auto& target=tags[key];if(!target.empty())target+=L"; ";target+=value;
}
void SetTitle(TrackInfo& i){
    auto title=i.current.tags.find("title"),artist=i.current.tags.find("artist");std::wstring name=title==i.current.tags.end()?L"":title->second;
    if(name.empty())name=i.station.name;if(name.empty())name=IsHttp(i.path.c_str())?RequestUrl(i.path.c_str()):std::filesystem::path(i.path).stem().wstring();
    i.title=artist!=i.current.tags.end()&&!artist->second.empty()?artist->second+L" - "+name:name;
    if(!i.station.title.empty())i.title=i.station.title;
}
std::wstring FormatInfo(const TrackInfo& i){
    std::wostringstream out;out<<CodecName(i.codec)<<L"\r\n"<<i.current.rate<<L" Hz, "<<i.current.channels<<L" channel(s)\r\n";
    if(i.current.bits_per_sample>0)out<<i.current.bits_per_sample<<L"-bit source PCM\r\n";
    if(!i.current.decoder_note.empty())out<<i.current.decoder_note<<L"\r\n";
    if(i.current.bitrate>0)out<<(static_cast<long long>(i.current.bitrate)+500)/1000<<L" kbps\r\n";
    if(i.duration_ms>=0)out<<i.duration_ms/60000<<L":"<<(i.duration_ms/1000%60<10?L"0":L"")<<i.duration_ms/1000%60<<L" ("<<i.duration_ms<<L" ms)\r\n";
    else out<<L"Live / unknown duration\r\n";
    out<<(i.seekable?L"Seekable":L"Forward-only")<<L"\r\nLogical streams: "<<i.links.size()<<L"\r\n";
    if(!i.current.vendor.empty())out<<L"Vendor: "<<i.current.vendor<<L"\r\n";
    if(!i.station.name.empty())out<<L"Station: "<<i.station.name<<L"\r\n";
    if(!i.station.description.empty())out<<i.station.description<<L"\r\n";
    if(!i.station.genre.empty())out<<L"Genre: "<<i.station.genre<<L"\r\n";
    if(!i.station.website.empty())out<<i.station.website<<L"\r\n";
    if(!i.station.recording_path.empty())out<<L"Recording: "<<i.station.recording_path<<L"\r\n";
    if(!i.station.recording_error.empty())out<<i.station.recording_error<<L"\r\n";return out.str();
}
bool Metadata(const TrackInfo& i,const char* raw,std::wstring& out){
    if(!raw)return false;std::string key(raw);std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(key=="type"){out=L"0";return true;}if(key=="rateable"){out=(!i.radio&&(i.codec==Codec::Vorbis||i.codec==Codec::Opus))?L"1":L"0";return true;}
    if(key=="mime"){out=L"audio/ogg";return true;}if(key=="serial"){out=std::to_wstring(i.current.serial);return true;}
    if(key=="gain"){out=std::to_wstring(std::pow(10.,i.current.header_gain_q8/(256.*20.)));return true;}
    if(key=="family"){out=CodecName(i.codec);return true;}if(key=="lossless"){out=i.codec==Codec::Flac?L"1":L"0";return true;}
    if(key=="length"){out=std::to_wstring(i.duration_ms);return true;}if(key=="bitrate"){out=std::to_wstring((static_cast<long long>(i.current.bitrate)+500)/1000);return true;}
    if(key=="samplerate"){out=std::to_wstring(i.current.rate);return true;}if(key=="channels"){out=std::to_wstring(i.current.channels);return true;}
    if(key=="formatinformation"){out=FormatInfo(i);out.erase(std::remove(out.begin(),out.end(),L'\r'),out.end());return true;}
    if(key=="replaygain_track_gain"||key=="replaygain_album_gain"){
        auto direct=i.current.tags.find(key);if(direct!=i.current.tags.end()){out=direct->second;return true;}
        if(i.codec==Codec::Opus){auto tag=i.current.tags.find(key=="replaygain_album_gain"?"r128_album_gain":"r128_track_gain");if(tag==i.current.tags.end())return false;auto text=ToUtf8(tag->second);int value=0;auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()||value<-32768||value>32767)return false;
            int q8=value+i.current.header_gain_q8+5*256;auto magnitude=static_cast<long long>(q8<0?-q8:q8);auto hundredths=(magnitude*100+128)/256;wchar_t formatted[64]{};swprintf_s(formatted,L"%ls%lld.%02lld",q8<0?L"-":L"+",hundredths/100,hundredths%100);out=formatted;return true;}
    }
    if(key=="stereo"){out=i.current.channels==1?L"Mono":i.current.channels==2?L"Stereo":L"Multichannel";return true;}
    if(key=="rating"){auto item=i.current.tags.find("rating");if(item==i.current.tags.end())return false;auto text=ToUtf8(item->second);int rating=0;auto parsed=std::from_chars(text.data(),text.data()+text.size(),rating);if(parsed.ec!=std::errc{})return false;rating=rating>5?(std::min(rating,100)+19)/20:std::clamp(rating,0,5);out=std::to_wstring(rating);return true;}
    if(key=="year")key="date";else if(key=="track")key="tracknumber";else if(key=="disc")key="discnumber";else if(key=="albumartist")key="albumartist";
    auto it=i.current.tags.find(key);if(it==i.current.tags.end()&&key=="comment")it=i.current.tags.find("description");
    if(it==i.current.tags.end()&&key=="albumartist")it=i.current.tags.find("album artist");
    if(it==i.current.tags.end()&&key=="publisher")it=i.current.tags.find("organization");
    if(it==i.current.tags.end()&&key=="category")it=i.current.tags.find("genre");
    if(it==i.current.tags.end()&&key=="grouping")it=i.current.tags.find("contentgroup");
    if(it==i.current.tags.end())return false;out=it->second;return true;
}
}
