// SPDX-License-Identifier: GPL-3.0-only
#include "backend.hpp"
#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>
namespace ogg_input {
namespace {
// A successful empty result means "supported, but unset" in Winamp's metadata
// ABI. Returning false for these fields disables their Basic Info controls.
bool SupportedTextField(const std::string& key) {
    for (const char* field : {"title", "artist", "album", "genre", "comment", "description",
            "year", "date", "track", "tracknumber", "disc", "discnumber", "bpm",
            "albumartist", "album artist", "ensemble", "composer", "publisher", "organization",
            "conductor", "tool", "encoded-by", "remixing", "subtitle", "version", "isrc",
            "category", "contentgroup", "grouping", "rating", "producer",
            "gracenotefileid", "gracenoteextdata", "replaygain_track_gain", "replaygain_track_peak",
            "replaygain_album_gain", "replaygain_album_peak"})
        if (key == field) return true;
    return false;
}

long double DurationSeconds(const TrackInfo& info) {
    // Native sample counts avoid millisecond truncation in container bitrate.
    long double seconds = 0;
    if (!info.links.empty()) {
        for (const auto& link : info.links) {
            if (link.frames < 0 || link.rate <= 0) return info.duration_ms / 1000.L;
            seconds += static_cast<long double>(link.frames) / link.rate;
        }
        return seconds;
    }
    return info.duration_ms / 1000.L;
}
}
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
std::wstring FormatInfo(const TrackInfo& info, MetadataText localize) {
    const auto text = [localize](const wchar_t* fallback) {
        return localize ? localize(fallback) : std::wstring(fallback);
    };
    const auto& link = info.current;
    std::wostringstream out;
    out.imbue(std::locale::classic());
    const long double seconds = DurationSeconds(info);
    if (info.duration_ms >= 0) {
        const int total = info.duration_ms / 1000;
        out << text(L"Length: ");
        if (total >= 3600) out << total / 3600 << L':' << std::setw(2) << std::setfill(L'0') << total / 60 % 60 << L':';
        else if (total >= 60) out << total / 60 << L':';
        if (total >= 60) out << std::setw(2) << std::setfill(L'0');
        out << total % 60 << L"\r\n";
    }
    if (info.file_size > 0 && seconds > 0) {
        // Historical File Info counts the complete container, including tags
        // and Ogg overhead, and truncates kbps rather than rounding audio bps.
        out << text(L"Average bitrate: ") << std::fixed << std::setprecision(0)
            << std::floor(static_cast<long double>(info.file_size) * 8 / (seconds * 1000))
            << L' ' << text(L"kbps") << L"\r\n" << std::defaultfloat;
    }
    if (info.file_size >= 0)
        out << text(L"File size: ") << info.file_size << L' ' << text(L"bytes") << L"\r\n";
    for (const auto& rate : {std::pair{L"Nominal bitrate: ", link.nominal_bitrate},
            {L"Maximum bitrate: ", link.maximum_bitrate}, {L"Minimum bitrate: ", link.minimum_bitrate}})
        if (rate.second > 0) out << text(rate.first) << rate.second / 1000 << L' ' << text(L"kbps") << L"\r\n";
    if (link.channels > 0) out << text(L"Channels: ") << link.channels << L"\r\n";
    if (link.rate > 0) out << text(L"Sampling rate: ") << link.rate << L' ' << text(L"Hz") << L"\r\n";
    out << text(L"Serial number: ") << link.serial;
    if (link.format_version >= 0) out << L"\r\n" << text(L"Version: ") << link.format_version;
    if (!link.vendor.empty()) out << L"\r\n" << text(L"Vendor:") << L"\r\n" << link.vendor;

    // Keep the original block first. Additional facts follow it, never invented
    // source bit depth for lossy codecs or a codec version inferred from tags.
    out << L"\r\n\r\n" << text(L"Codec: ") << CodecName(info.codec);
    if (info.duration_ms >= 0) out << L"\r\n" << text(L"Duration: ") << info.duration_ms << L" ms";
    if (link.frames >= 0) out << L"\r\n" << text(L"Samples per channel: ") << link.frames;
    if (link.bits_per_sample > 0) out << L"\r\n" << text(L"Source bit depth: ") << link.bits_per_sample;
    if (info.links.size() > 1)
        out << L"\r\n" << text(L"Logical stream: ") << info.current_link + 1 << L" / " << info.links.size();
    out << L"\r\n" << text(L"Seekable: ") << text(info.seekable ? L"Yes" : L"No");
    if (info.codec == Codec::Opus)
        out << L"\r\n" << text(L"Header gain: ") << std::fixed << std::setprecision(2)
            << std::showpos << link.header_gain_q8 / 256. << L" dB" << std::noshowpos << std::defaultfloat;
    if (!link.decoder_note.empty()) out << L"\r\n" << link.decoder_note;
    if (!info.station.name.empty()) out << L"\r\n" << text(L"Station: ") << info.station.name;
    if (!info.station.description.empty()) out << L"\r\n" << info.station.description;
    if (!info.station.genre.empty()) out << L"\r\n" << text(L"Genre: ") << info.station.genre;
    if (!info.station.website.empty()) out << L"\r\n" << info.station.website;
    if (!info.station.recording_path.empty()) out << L"\r\n" << text(L"Recording: ") << info.station.recording_path;
    if (!info.station.recording_error.empty()) out << L"\r\n" << info.station.recording_error;
    return out.str();
}
bool Metadata(const TrackInfo& i,const char* raw,std::wstring& out,MetadataText localize){
    out.clear();
    if(!raw)return false;std::string key(raw);std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    if(key=="type"){out=L"0";return true;}if(key=="rateable"){out=(!i.radio&&(i.codec==Codec::Vorbis||i.codec==Codec::Opus))?L"1":L"0";return true;}
    if(key=="mime"){out=L"audio/ogg";return true;}if(key=="serial"||key=="serialnumber"){out=std::to_wstring(i.current.serial);return true;}
    if(key=="serialnumber_hex"){wchar_t value[9]{};swprintf_s(value,L"%08x",i.current.serial);out=value;return true;}
    if(key=="gain"){out=std::to_wstring(std::pow(10.,i.current.header_gain_q8/(256.*20.)));return true;}
    if(key=="family"){out=CodecName(i.codec);return true;}if(key=="lossless"){out=i.codec==Codec::Flac?L"1":L"0";return true;}
    if(key=="length"){out=std::to_wstring(i.duration_ms);return true;}if(key=="bitrate"){out=std::to_wstring((static_cast<long long>(i.current.bitrate)+500)/1000);return true;}
    if(key=="samplerate"){out=std::to_wstring(i.current.rate);return true;}if(key=="channels"){out=std::to_wstring(i.current.channels);return true;}
    if(key=="formatinformation"){out=FormatInfo(i,localize);return true;}
    if(key=="replaygain_track_gain"||key=="replaygain_album_gain"){
        auto direct=i.current.tags.find(key);if(direct!=i.current.tags.end()){out=direct->second;return true;}
        if(i.codec==Codec::Opus){auto tag=i.current.tags.find(key=="replaygain_album_gain"?"r128_album_gain":"r128_track_gain");if(tag==i.current.tags.end())return true;auto text=ToUtf8(tag->second);int value=0;auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()||value<-32768||value>32767)return true;
            int q8=value+i.current.header_gain_q8+5*256;auto magnitude=static_cast<long long>(q8<0?-q8:q8);auto hundredths=(magnitude*100+128)/256;wchar_t formatted[64]{};swprintf_s(formatted,L"%ls%lld.%02lld",q8<0?L"-":L"+",hundredths/100,hundredths%100);out=formatted;return true;}
    }
    if(key=="stereo"){out=i.current.channels==1?L"Mono":i.current.channels==2?L"Stereo":L"Multichannel";return true;}
    if(key=="rating"){auto item=i.current.tags.find("rating");if(item==i.current.tags.end())return true;auto text=ToUtf8(item->second);int rating=0;auto parsed=std::from_chars(text.data(),text.data()+text.size(),rating);if(parsed.ec!=std::errc{})return true;rating=rating>5?(std::min(rating,100)+19)/20:std::clamp(rating,0,5);out=std::to_wstring(rating);return true;}
    if(key=="year")key="date";else if(key=="track")key="tracknumber";else if(key=="disc")key="discnumber";
    auto it=i.current.tags.find(key);
    const auto fallback=[&](const char* alias){
        if(it==i.current.tags.end()||it->second.empty()){
            const auto candidate=i.current.tags.find(alias);
            if(candidate!=i.current.tags.end())it=candidate;
        }
    };
    if(key=="comment")fallback("description");
    if(key=="albumartist"){fallback("album artist");fallback("ensemble");}
    if(key=="date")fallback("year");
    if(key=="tracknumber")fallback("track");
    if(key=="discnumber")fallback("disc");
    if(key=="tool")fallback("encoded-by");
    if(key=="subtitle")fallback("version");
    if(key=="publisher")fallback("organization");
    if(key=="category"){fallback("contentgroup");fallback("grouping");}
    if(key=="grouping")fallback("contentgroup");
    if(it==i.current.tags.end())return SupportedTextField(key);out=it->second;return true;
}
bool MetadataPatch(const TrackInfo& info,const char* key,const wchar_t* value,Tags& patch){
    if(!key||!value||info.radio||(info.codec!=Codec::Vorbis&&info.codec!=Codec::Opus))return false;
    std::string name(key);for(char& c:name){if(c<32||c>125||c=='=')return false;c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));}
    if(name.empty())return false;
    if(name=="year")name="date";else if(name=="track")name="tracknumber";else if(name=="disc")name="discnumber";
    else if(name=="tool")name="encoded-by";else if(name=="subtitle")name="version";
    for(const auto* technical:{"length","bitrate","channels","samplerate","type","lossless","family","formatinformation","mime","rateable","serial","serialnumber","serialnumber_hex","gain"})if(name==technical)return false;
    std::wstring stored=value;std::string obsolete;
    if(name=="rating"&&*value){auto text=ToUtf8(stored);int rating=0;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),rating);
        if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()||rating<0||rating>5)return false;stored=std::to_wstring(rating*20);}
    if(info.codec==Codec::Opus&&(name=="replaygain_track_gain"||name=="replaygain_album_gain")){
        obsolete=name;name=name=="replaygain_album_gain"?"r128_album_gain":"r128_track_gain";
        if(*value){auto text=ToUtf8(stored);auto first=text.data();const auto end=first+text.size();
            while(first!=end&&(*first==' '||*first=='\t'))++first;if(first!=end&&*first=='+')++first;
            double db=0;const auto parsed=std::from_chars(first,end,db);if(parsed.ec!=std::errc{}||!std::isfinite(db))return false;
            auto tail=parsed.ptr;while(tail!=end&&(*tail==' '||*tail=='\t'))++tail;
            if(end-tail>=2&&(tail[0]=='d'||tail[0]=='D')&&(tail[1]=='b'||tail[1]=='B'))tail+=2;
            while(tail!=end&&(*tail==' '||*tail=='\t'))++tail;if(tail!=end)return false;
            const double q8=db*256.-info.current.header_gain_q8-5*256.;if(!std::isfinite(q8)||q8<-32768||q8>32767)return false;
            stored=std::to_wstring(static_cast<int>(std::lround(q8)));}
    }
    if(!obsolete.empty())patch[obsolete]=L"";patch[name]=std::move(stored);return true;
}
}
