#include "audio.hpp"
#include "codec_fixtures.hpp"
#include "tag_writer.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
using namespace ogg_input;
namespace {
std::int64_t Drain(Audio& a,Settings settings,std::vector<float>* floats=nullptr){
    if(floats){std::array<float,BlockFrames*MaxChannels> data{};std::int64_t frames=0;for(;;){int n=a.Read(data.data(),BlockFrames);Check(n>=0&&n<=BlockFrames,"float read capacity");if(!n)return frames;floats->insert(floats->end(),data.begin(),data.begin()+n*a.Channels());frames+=n;}}
    Pcm pipeline(a,settings);Check(pipeline.Ready(),"PCM ready");std::array<std::uint32_t,BlockFrames*MaxChannels+2> buffer{};auto* data=reinterpret_cast<unsigned char*>(buffer.data()+1);int size=BlockFrames*a.Channels()*settings.bits/8;std::int64_t frames=0;
    for(;;){buffer[0]=0x12345678;memset(data+size,0xa5,4);int n=pipeline.Read(data,BlockFrames);Check(n>=0&&n<=BlockFrames,"PCM read capacity");Check(buffer[0]==0x12345678,"lower PCM guard");for(int i=0;i<4;++i)Check(data[size+i]==0xa5,"upper PCM guard");if(!n)return frames;frames+=n;}
}
void LocalSeeks(const std::filesystem::path& path,int rate,int frames,bool buffered=false){
    Settings s;s.buffer_small_files=buffered;Audio a;std::wstring error;
    Check(a.Open(path.c_str(),s,false,error),"open local seek fixture");
    const int duration=static_cast<int>(frames*1000LL/rate);
    std::cout<<ToUtf8(path.filename().wstring())<<": startup duration="<<a.Info().duration_ms<<" ms, buffered="<<buffered<<'\n';
    Check(a.Info().seekable&&!a.Info().radio&&a.Info().duration_ms==duration,"local duration available before first read");
    Check(a.Info().links.size()==1&&a.Info().current.frames==frames,"local startup link sample count");
    const auto metadata=a.Info().current;
    if(a.Info().codec==Codec::Speex)Check(metadata.tags.at("title")==L"Speex fixture","Speex indexed comments available at startup");
    if(a.Info().codec==Codec::Flac){
        Check(metadata.tags.at("title")==L"FLAC fixture","FLAC indexed comments available at startup");
        ByteStream source;Check(source.Open(path.c_str(),s,error),"open FLAC bitrate reference");
        const int expected=static_cast<int>(static_cast<long double>(source.Size())*8*rate/frames);
        std::cout<<"FLAC average bitrate: "<<metadata.bitrate<<" bps, expected "<<expected<<" bps\n";
        Check(metadata.bitrate==expected&&metadata.bitrate>1000,"FLAC startup bitrate uses compressed Ogg bytes, not PCM size");
    }
    Check(Drain(a,s)==frames,"indexed local file preserves all PCM");
    for(int ms:{duration*3/4,duration/4,0,duration/2}){
        Check(a.Seek(ms),"local repeated forward/backward seek");
        const auto remaining=Drain(a,s);
        Check(std::abs(remaining-(frames-rate*static_cast<long long>(ms)/1000))<=1,"local seek remaining sample count");
        Check(a.Info().duration_ms==duration&&a.Info().links.size()==1&&a.Info().current.frames==frames,
              "local seek retains indexed duration and links");
        Check(a.Info().current.bitrate==metadata.bitrate,"local seek preserves average compressed bitrate");
        Check(a.Info().current.tags==metadata.tags&&a.Info().current.vendor==metadata.vendor,"local seek preserves metadata");
    }
    Check(a.Seek(duration+1000),"local seek past EOF succeeds");
    std::array<float,BlockFrames*MaxChannels> data{};
    Check(a.Read(data.data(),BlockFrames)==0,"local seek past EOF leaves no PCM");
    Check(a.Seek(0)&&Drain(a,s)==frames,"local rewind after EOF");
}
}
int wmain(int argc,wchar_t** argv){try{
    if(argc==3&&std::wstring(argv[1])==L"--radio"){Audio a;Settings s;std::wstring error;if(!a.Open(argv[2],s,false,error))throw std::runtime_error(ToUtf8(error));Check(a.Info().radio&&!a.Info().seekable,"HTTP radio flags");Pcm pcm(a,s);std::array<short,BlockFrames*2> samples{};int frames=0;for(int i=0;i<30;++i){int n=pcm.Read(samples.data(),BlockFrames);Check(n>0,"live radio decode");frames+=n;}std::wcout<<CodecName(a.Info().codec)<<L": "<<frames<<L" frames, "<<a.Info().station.name<<L"\n";return 0;}
    if(argc==3&&std::wstring(argv[1])==L"--file"){
        Audio a;Settings s;std::wstring error;Check(a.Open(argv[2],s,false,error),ToUtf8(error).c_str());
        const int duration=a.Info().duration_ms;
        std::wcout<<CodecName(a.Info().codec)<<L": duration="<<duration<<L" ms, seekable="<<a.Info().seekable<<L", rate="<<a.SourceRate()<<L", bitrate="<<a.Info().current.bitrate<<L" bps\n";
        if(a.Info().codec==Codec::Flac)Check(a.Info().current.bitrate>1000,"external FLAC bitrate available before playback");
        Check(a.Info().seekable&&duration>0,"local duration available before playback");
        std::array<float,BlockFrames*MaxChannels> data{};
        for(int ms:{duration*3/4,duration/4,0}){
            Check(a.Seek(ms),"external file time seek");Check(a.Read(data.data(),BlockFrames)>0,"external file PCM after seek");
            Check(a.Info().duration_ms==duration,"external file duration retained after seek");
            std::cout<<"Seek to "<<ms<<" ms: PCM available\n";
        }
        Check(a.Seek(duration+1)&&a.Read(data.data(),BlockFrames)==0,"external file seek past EOF");
        return 0;
    }
    Check(argc==2,"usage: ogg_tests DIRECTORY | --radio URL | --file FILE");auto directory=std::filesystem::path(argv[1]);std::filesystem::create_directories(directory);
    constexpr int frames=7013;auto opus=directory/L"Opus｜test.ogg",vorbis=directory/L"Vorbis.ogg",flac=directory/L"FLAC.oga",speex=directory/L"Speex.spx";
    MakeOpus(opus,2,frames,2001);MakeVorbis(vorbis,2,44100,frames,2002);MakeFlac(flac,2,44100,frames);MakeSpeex(speex,16000,frames);
    std::vector<std::filesystem::path> paths{opus,vorbis,flac};
#ifdef OGG_HAVE_SPEEX
    paths.push_back(speex);
#endif
    for(auto path:paths){
        Settings s;Audio a;std::wstring error;Check(a.Open(path.c_str(),s,false,error),"open local seek matrix");auto source_rate=a.SourceRate();LocalSeeks(path,source_rate,frames);LocalSeeks(path,source_rate,frames,true);
        Check(a.Open(path.c_str(),s,false,error),"reopen seek fixture");Check(a.Seek(50),"seek local codec");auto remaining=Drain(a,s);auto expected=frames-static_cast<std::int64_t>(source_rate)*50/1000;Check(std::abs(remaining-expected)<=1,"seek remaining sample count");
        for(int rate:{8000,48000,96000})for(int bits:{8,16,24,32}){s.rate=rate;s.bits=bits;Check(a.Open(path.c_str(),s,false,error),"format fixture reopen");auto n=Drain(a,s);auto target=static_cast<std::int64_t>(frames)*rate/source_rate;Check(std::abs(n-target)<=1,"resampled sample count");}
        std::cout<<ToUtf8(path.filename().wstring())<<": native, seek, integer formats, resampling passed\n";
    }
    for(auto path:{opus,vorbis}){
        Audio a;Settings s;std::wstring error;Check(a.Open(path.c_str(),s,false,error),"tag baseline open");std::vector<float> before;Drain(a,s,&before);auto tags=a.Info().current.tags;tags["title"]=L"Edited Unicode title — テスト";tags["custom"]=std::wstring(60000,L'x');
        if(!WriteTags(path.c_str(),0,tags,error))throw std::runtime_error(ToUtf8(error));Check(a.Open(path.c_str(),s,false,error),"open rewritten tags");Check(a.Info().current.tags.at("title")==tags["title"],"rewritten title");std::vector<float> after;Drain(a,s,&after);Check(before==after,"tag edit must preserve decoded audio exactly");
        Tags deletion;deletion["custom"]=L"";if(!WriteTags(path.c_str(),0,deletion,error))throw std::runtime_error(ToUtf8(error));Check(a.Open(path.c_str(),s,false,error),"reopen deleted tag");Check(a.Info().current.tags.find("custom")==a.Info().current.tags.end(),"empty patch deletes tag");Check(a.Info().current.tags.at("title")==tags["title"],"unrelated tags survive deletion");
    }
    {auto chain=directory/L"chained.ogg",tail=directory/L"tail.ogg";MakeVorbis(chain,6,44100,frames,3001);MakeVorbis(tail,2,48000,frames,3002);AppendFile(chain,tail);
    Audio chained;Settings s;std::wstring error;Check(chained.Open(chain.c_str(),s,false,error),"open heterogeneous chain");Check(chained.Channels()==2,"chain channel fallback");Check(chained.Info().links.size()==2,"chain link metadata");auto count=Drain(chained,s);auto chain_expected=frames+static_cast<std::int64_t>(frames)*44100/48000;std::cout<<"Chained output: "<<count<<", expected "<<chain_expected<<", rate "<<chained.Rate()<<"\n";Check(std::abs(count-chain_expected)<=2,"mixed-rate chain resampled duration");}
    {auto chain=directory/L"opus-chain.ogg",tail=directory/L"opus-tail.ogg";
        MakeOpus(chain,6,frames,3201);MakeOpus(tail,2,frames,3202);AppendFile(chain,tail);
        Audio a;Settings s;std::wstring error;Check(a.Open(chain.c_str(),s,false,error),"Opus chain open");
        Check(a.Channels()==2&&a.Info().links.size()==2,"Opus mixed-layout chain metadata");
        Check(Drain(a,s)==2*frames,"Opus chain complete output");
    }
    {auto chain=directory/L"flac-chain.oga",tail=directory/L"flac-tail.oga";
        MakeFlac(chain,2,44100,frames);MakeFlac(tail,2,44100,frames);AppendFile(chain,tail);
        Audio a;Settings s;std::wstring error;Check(a.Open(chain.c_str(),s,false,error),"FLAC chain open");
        Check(Drain(a,s)==2*frames,"FLAC chain complete output");
        Check(a.Info().links.size()==2,"FLAC discovers chained metadata");
    }
    for(int rate:{44100,96000}){
        auto path=directory/(L"unknown-flac-"+std::to_wstring(rate)+L".oga");MakeFlac(path,2,rate,96001,false);
        LocalSeeks(path,rate,96001);LocalSeeks(path,rate,96001,true);
    }
    for(bool known_total:{false,true}){
        auto chain=directory/(known_total?L"known-flac-chain.oga":L"unknown-flac-chain.oga"),tail=directory/L"flac-seek-tail.oga";
        MakeFlac(chain,2,44100,44100,known_total);MakeFlac(tail,1,96000,192000,known_total);
        const auto first_bitrate=static_cast<int>(std::filesystem::file_size(chain)*8);
        const auto tail_bitrate=static_cast<int>(std::filesystem::file_size(tail)*4);AppendFile(chain,tail);
        Audio a;Settings s;std::wstring error;Check(a.Open(chain.c_str(),s,false,error),"open mixed-rate FLAC seek chain");
        Check(a.Info().links.size()==2&&a.Info().duration_ms==3000,"complete FLAC chain duration before first read");
        Check(a.Info().links[0].frames==44100&&a.Info().links[1].frames==192000,"FLAC chained native sample counts");
        Check(a.Info().links[0].bitrate==first_bitrate&&a.Info().links[1].bitrate==tail_bitrate,"FLAC chain bitrates count each link's own Ogg bytes");
        for(int ms:{1500,250,2500,1000,0}){
            Check(a.Seek(ms),"FLAC repeated seek across mixed-rate links");
            Check(std::abs(Drain(a,s)-(3000-ms)*44100LL/1000)<=1,"FLAC chained time seek uses per-link rates");
            Check(a.Info().duration_ms==3000&&a.Info().links.size()==2,"FLAC complete index survives seeks");
        }
        Check(a.Seek(4000),"FLAC chained seek past EOF");std::array<float,BlockFrames*MaxChannels> data{};
        Check(a.Read(data.data(),BlockFrames)==0,"FLAC chained EOF is empty");
        Check(a.Seek(0)&&Drain(a,s)==132300,"FLAC chained rewind after EOF");
    }
#ifdef OGG_HAVE_SPEEX
    for(int rate:{8000,16000,32000}){
        auto path=directory/(L"seek-speex-"+std::to_wstring(rate)+L".spx");MakeSpeex(path,rate,96001,3350+rate,50);
        LocalSeeks(path,rate,96001);LocalSeeks(path,rate,96001,true);
    }
    {auto chain=directory/L"speex-chain.spx",tail=directory/L"speex-tail.spx";
        MakeSpeex(chain,16000,frames,3301);MakeSpeex(tail,8000,frames,3302);AppendFile(chain,tail);
        Audio a;Settings s;std::wstring error;Check(a.Open(chain.c_str(),s,false,error),"Speex chain open");
        const int duration=frames*1000/16000+frames*1000/8000;
        Check(a.Info().links.size()==2&&a.Info().duration_ms==duration,"Speex complete chain duration at startup");
        Check(a.Info().links[0].frames==frames&&a.Info().links[1].frames==frames,"Speex indexed chained sample counts");
        Check(std::abs(Drain(a,s)-3*frames)<=2,"Speex mixed-rate chain complete output");
        for(int ms:{500,100,1000,0}){
            Check(a.Seek(ms),"Speex seek across mixed-rate links");
            Check(std::abs(Drain(a,s)-(3*frames-ms*16LL))<=2,"Speex mixed-rate seek output duration");
            Check(a.Info().links.size()==2&&a.Info().duration_ms==duration,"Speex seek retains complete chain index");
        }
    }
#endif
    {auto path=directory/L"replaygain.ogg";MakeVorbis(path,2,48000,frames,3401);
        Audio a;Settings s;std::wstring error;Check(a.Open(path.c_str(),s,false,error),"gain baseline open");
        std::vector<float> baseline;Drain(a,s,&baseline);
        Tags tags;tags["replaygain_track_gain"]=L"+6.00 dB";tags["replaygain_album_gain"]=L"+3.00 dB";
        tags["replaygain_track_peak"]=L"0.1";tags["replaygain_album_peak"]=L"0.5";
        Check(WriteTags(path.c_str(),0,tags,error),"write ReplayGain fixture");
        auto verify_gain=[&](double multiplier){Check(a.Open(path.c_str(),s,false,error),"gain fixture open");std::vector<float> output;Drain(a,s,&output);Check(output.size()==baseline.size(),"gain preserves duration");
            for(size_t i=0;i<output.size();++i)Check(std::abs(output[i]-baseline[i]*multiplier)<0.00001,"ReplayGain multiplier");};
        s.gain=GainMode::Track;s.target_lufs=-18;verify_gain(std::pow(10.,6./20.));
        s.gain=GainMode::Album;verify_gain(std::pow(10.,3./20.));
        s.peak_mode=2;verify_gain(2.);s.peak_mode=3;verify_gain(1.);
        tags.clear();tags["replaygain_track_gain"]=L"";Check(WriteTags(path.c_str(),0,tags,error),"remove preferred gain");
        s.gain=GainMode::Track;s.peak_mode=0;verify_gain(std::pow(10.,3./20.));
        s.preferred_gain_only=true;verify_gain(std::pow(10.,-6./20.));
        s.preferred_gain_only=false;s.peak_mode=2;verify_gain(2.);
        tags.clear();tags["replaygain_track_peak"]=L"";tags["replaygain_album_peak"]=L"";
        Check(WriteTags(path.c_str(),0,tags,error),"remove peak tags");
        s.peak_mode=3;verify_gain(1.);
    }
    {auto path=directory/L"opus-host-gain.ogg";MakeOpus(path,2,frames,3450,{},256);
        Audio a;Settings s;std::wstring error;std::vector<float> baseline;
        Check(a.Open(path.c_str(),s,false,error),"Opus header gain baseline");Drain(a,s,&baseline);
        auto verify=[&](double ratio){Check(a.Open(path.c_str(),s,false,error),"Opus host gain reopen");std::vector<float> output;Drain(a,s,&output);
            Check(output.size()==baseline.size(),"Opus gain duration");for(size_t i=0;i<output.size();++i)Check(std::abs(output[i]-baseline[i]*ratio)<0.00001,"Opus host gain multiplier");};
        s.gain=GainMode::Track;s.non_rg_q8=-6*256;verify(std::pow(10.,-6./20.));
        s.peak_mode=3;verify(1.); // Mandatory header gain survives a peak-only policy.
        s.gain=GainMode::Header;s.preamp_q8=0;verify(1.); // Host RG disabled.
    }
    {auto chain=directory/L"tag-chain.ogg",tail=directory/L"tag-tail.ogg";
        MakeVorbis(chain,2,44100,frames,3101);MakeVorbis(tail,2,44100,frames,3102);AppendFile(chain,tail);
        Audio a;Settings s;std::wstring error;Check(a.Open(chain.c_str(),s,false,error),"tag chain open");
        std::vector<float> before;Drain(a,s,&before);Check(before.size()==frames*4,"tag chain complete baseline");
        Tags patch;patch["title"]=L"First link only";patch["custom"]=L"Selected";
        if(!WriteTags(chain.c_str(),0,patch,error))throw std::runtime_error(ToUtf8(error));
        Check(a.Open(chain.c_str(),s,false,error),"selected-link tag reopen");
        Check(a.Info().links[0].tags.at("title")==L"First link only","selected link edited");
        Check(a.Info().links[1].tags.at("title")==L"Vorbis fixture","other link unchanged");
        std::vector<float> selected;Drain(a,s,&selected);Check(before==selected,"selected chain tags preserve audio");
        patch.clear();patch["artist"]=L"All links";patch["custom"]=L"";
        if(!WriteTags(chain.c_str(),-1,patch,error))throw std::runtime_error(ToUtf8(error));
        Check(a.Open(chain.c_str(),s,false,error),"all-link tag reopen");
        for(const auto& link:a.Info().links){Check(link.tags.at("artist")==L"All links","all links edited");Check(link.tags.find("custom")==link.tags.end(),"chain tombstone");}
        std::vector<float> all;Drain(a,s,&all);Check(before==all,"all chain tags preserve audio");
    }
    {auto bad=directory/L"invalid-crc.ogg";std::ifstream in(vorbis,std::ios::binary);
        std::vector<char> original{std::istreambuf_iterator<char>(in),{}};Check(original.size()>100,"corrupt fixture size");
        original.back()^=1;{std::ofstream out(bad,std::ios::binary);out.write(original.data(),original.size());}
        std::wstring error;Tags patch;patch["title"]=L"Must not be written";
        Check(!WriteTags(bad.c_str(),0,patch,error)&&!error.empty(),"reject CRC-damaged tag edit");
        std::ifstream unchanged(bad,std::ios::binary);std::vector<char> after{std::istreambuf_iterator<char>(unchanged),{}};
        Check(original==after,"failed tag edit leaves source unchanged");
    }
    {std::wstring repaired=opus.wstring();std::replace(repaired.begin(),repaired.end(),L'｜',L'|');Audio a;Settings s;std::wstring error;Check(a.Open(repaired.c_str(),s,false,error),"read-only Unicode path fallback");Check(Drain(a,s)==frames,"Unicode fallback PCM");}
    for(int channels=1;channels<=8;++channels){auto path=directory/(L"channels-"+std::to_wstring(channels)+L".ogg");MakeVorbis(path,channels,48000,frames,6000+channels);for(auto mode:{ChannelMode::Source,ChannelMode::Stereo,ChannelMode::Mono,ChannelMode::Quad,ChannelMode::LegacyUnmapped,ChannelMode::Surround,ChannelMode::SurroundII}){Settings s;s.channels=mode;Audio a;std::wstring error;Check(a.Open(path.c_str(),s,false,error),"multichannel Vorbis open");Check(Drain(a,s)==frames,"multichannel output frame count");}}
    {auto path=directory/L"order-six.ogg";MakeVorbis(path,6,48000,frames,6100);
        Audio a;Settings s;std::wstring error;std::vector<float> windows,raw,legacy;
        Check(a.Open(path.c_str(),s,false,error),"Windows-order fixture");Drain(a,s,&windows);
        s.channels=ChannelMode::LegacyUnmapped;Check(a.Open(path.c_str(),s,false,error),"raw codec-order fixture");Drain(a,s,&raw);
        s.channels=ChannelMode::Source;s.legacy_51_order=true;Check(a.Open(path.c_str(),s,false,error),"legacy input-order fixture");Drain(a,s,&legacy);
        constexpr int map[]={0,2,1,5,3,4};
        for(size_t i=0;i<windows.size();i+=6)for(int c=0;c<6;++c){
            Check(windows[i+c]==raw[i+map[c]],"codec to Windows channel mapping");
            const int old=c==1?2:c==2?1:c;Check(legacy[i+c]==windows[i+old],"legacy six-channel input mapping");
        }
    }
    for(auto path:paths){
        auto wrapper=directory/(path.stem().wstring()+L"-wrapped"+path.extension().wstring());
        std::ifstream in(path,std::ios::binary);std::vector<char> payload{std::istreambuf_iterator<char>(in),{}};
        std::ofstream out(wrapper,std::ios::binary);auto le=[&](unsigned n){for(int b=0;b<4;++b)out.put(static_cast<char>(n>>(b*8)));};
        out.write("RIFF",4);le(static_cast<unsigned>(payload.size()+12));out.write("WAVEdata",8);le(static_cast<unsigned>(payload.size()));out.write(payload.data(),payload.size());out.close();
        Audio a;Settings s;std::wstring error;Check(a.Open(wrapper.c_str(),s,false,error),"RIFF/WAVE embedded Ogg");
        LocalSeeks(wrapper,a.SourceRate(),frames);LocalSeeks(wrapper,a.SourceRate(),frames,true);
    }
    std::cout<<"All codec and pipeline tests passed.\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
