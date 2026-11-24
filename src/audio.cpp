// SPDX-License-Identifier: GPL-3.0-only
#include "audio.hpp"
#include "channel_layout.hpp"
#include "processing.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <climits>
#include <cstring>
#include <utility>
namespace ogg_input {
namespace {
double Number(const Tags& tags,const char* name,double fallback){
    const auto item=tags.find(name);if(item==tags.end())return fallback;
    const auto text=ToUtf8(item->second);auto first=text.data();const auto end=first+text.size();
    while(first!=end&&(*first==' '||*first=='\t'))++first;
    if(first!=end&&*first=='+')++first;
    double number=0;const auto parsed=std::from_chars(first,end,number);
    if(parsed.ec!=std::errc{}||!std::isfinite(number))return fallback;
    auto tail=parsed.ptr;while(tail!=end&&(*tail==' '||*tail=='\t'))++tail;
    if(end-tail>=2&&(tail[0]=='d'||tail[0]=='D')&&(tail[1]=='b'||tail[1]=='B'))tail+=2;
    while(tail!=end&&(*tail==' '||*tail=='\t'))++tail;
    return tail==end?number:fallback;
}
}
bool Audio::Open(const wchar_t* path,Settings s,bool shuffle,std::wstring& error,bool negotiate){
    last_exact_=false;reader_.reset();{std::lock_guard<std::mutex> guard(stream_mutex_);stream_=std::make_shared<ByteStream>();}settings_=Validate(s);shuffle_=shuffle;negotiate_=negotiate&&settings_.native_chains;gain_link_=-1;error_.clear();
    if(cancelled_){error=L"Playback was cancelled.";return false;}
    if(!stream_->Open(path,settings_,error))return false;auto codec=Probe(*stream_);
    switch(codec){case Codec::Opus:reader_=OpenOpus(stream_,settings_,error);break;case Codec::Vorbis:reader_=OpenVorbis(stream_,settings_,error);break;case Codec::Flac:reader_=OpenFlac(stream_,settings_,error);break;case Codec::Speex:
#ifdef OGG_HAVE_SPEEX
        reader_=OpenSpeex(stream_,settings_,error);break;
#else
        error=L"This build does not include Speex decoding.";return false;
#endif
    default:error=L"Not a supported Ogg Vorbis, Opus, FLAC, or Speex stream.";return false;}
    if(!reader_)return false;stream_->FinishProbe();info_=reader_->Info();info_.path=path;info_.file_size=stream_->SourceSize();info_.station=stream_->Info();info_.radio=stream_->Radio();
    source_rate_=info_.current.rate;if(source_rate_<1000||source_rate_>768000||info_.current.channels<1||info_.current.channels>MaxChannels){error=L"Unsupported audio rate or channel count.";reader_.reset();return false;}
    channels_=OutputChannels(settings_.channels,info_.current.channels);
    if(!negotiate_&&(settings_.channels==ChannelMode::Source||settings_.channels==ChannelMode::LegacyUnmapped)){
        if(info_.radio)channels_=2;
        for(const auto& link:info_.links)if(link.channels!=channels_)channels_=2;
    }
    rate_=settings_.rate?settings_.rate:source_rate_;SetTitle(info_);Gain();++metadata_revision_;return true;
}
void Audio::Gain(){
    const auto& tags=info_.current.tags;double db=settings_.preamp_q8/256.;if(info_.radio)db=settings_.radio_preamp_q8/256.;
    auto mode=settings_.gain;if(mode==GainMode::Automatic)mode=shuffle_?GainMode::Track:GainMode::Album;
    double peak=0.;bool peak_album=mode==GainMode::Album;
    if(info_.codec==Codec::Opus){
        if(mode!=GainMode::Raw)db+=info_.current.header_gain_q8/256.;
        const char* key=mode==GainMode::Album?"r128_album_gain":"r128_track_gain";
        if(mode==GainMode::Album||mode==GainMode::Track){double tag=Number(tags,key,HUGE_VAL);
            if(!std::isfinite(tag)&&!settings_.preferred_gain_only){tag=Number(tags,mode==GainMode::Album?"r128_track_gain":"r128_album_gain",HUGE_VAL);if(std::isfinite(tag))peak_album=!peak_album;}
            if(std::isfinite(tag))db+=std::clamp(tag/256.,-128.,128.)+settings_.target_lufs+23;
            else db+=settings_.non_rg_q8/256.;
        }
    }else if(mode==GainMode::Album||mode==GainMode::Track){
        bool album=mode==GainMode::Album;
        auto tag_gain=[&](bool a){double value=Number(tags,a?"replaygain_album_gain":"replaygain_track_gain",HUGE_VAL);if(!std::isfinite(value))value=Number(tags,a?"rg_audiophile":"rg_radio",HUGE_VAL);return value;};
        double tag=tag_gain(album);if(!std::isfinite(tag)&&!settings_.preferred_gain_only){album=!album;tag=tag_gain(album);}
        peak_album=album;
        if(std::isfinite(tag))db+=std::clamp(tag,-128.,128.)+settings_.target_lufs+18;
        else{double legacy=Number(tags,"lwing_gain",0.);if(legacy>0&&legacy<=1000)db+=20*std::log10(legacy);else db+=settings_.non_rg_q8/256.;}
    }
    if(mode==GainMode::Album||mode==GainMode::Track){
        peak=Number(tags,peak_album?"replaygain_album_peak":"replaygain_track_peak",0.);
        if(peak<=0&&!settings_.preferred_gain_only)peak=Number(tags,peak_album?"replaygain_track_peak":"replaygain_album_peak",0.);
        if(peak<=0)peak=Number(tags,"rg_peak",0.);
    }
    gain_=std::pow(10.,std::clamp(db,-180.,90.)/20.);
    if((mode==GainMode::Album||mode==GainMode::Track)&&peak>0&&std::isfinite(peak)){
        if(settings_.peak_mode==2)gain_=std::pow(10.,(info_.radio?settings_.radio_preamp_q8:settings_.preamp_q8)/(256.*20.))/peak;
        else if(settings_.peak_mode==3)gain_=peak>1.?1./peak:1.;
    }
    if((mode==GainMode::Album||mode==GainMode::Track)&&settings_.peak_mode==3){
        const double header=info_.codec==Codec::Opus?std::pow(10.,info_.current.header_gain_q8/(256.*20.)):1.;
        gain_=peak>0&&std::isfinite(peak)?std::min(header,1./peak):header;
    }
    if(settings_.prevent_clipping&&settings_.peak_mode==1&&peak>0&&std::isfinite(peak)&&gain_*peak>1.)gain_=1./peak;gain_link_=info_.current_link;
}
int Audio::Read(float* out,int capacity){
    last_exact_=false;
    if(!reader_||!out||capacity<1||capacity>BlockFrames)return -1;int n=reader_->Read(scratch_.data(),capacity);
    if(n<=0){const auto duration=reader_->Info().duration_ms;if(info_.duration_ms!=duration){info_.duration_ms=duration;++metadata_revision_;}return n;}
    const auto& next=reader_->Info();
    const bool link_changed=next.current_link!=info_.current_link;
    bool changed=link_changed||next.current.rate!=info_.current.rate||next.current.channels!=info_.current.channels||
        next.current.bitrate!=info_.current.bitrate||next.current.frames!=info_.current.frames;
    if(changed){auto path=std::move(info_.path);auto title=std::move(info_.title);auto station=std::move(info_.station);info_=next;info_.path=std::move(path);info_.file_size=stream_->SourceSize();info_.title=std::move(title);info_.station=std::move(station);info_.radio=stream_->Radio();}
    bool title_changed=link_changed;
    if(info_.duration_ms!=next.duration_ms){info_.duration_ms=next.duration_ms;changed=true;}
    if(info_.station.title!=stream_->Info().title||info_.station.recording_error!=stream_->Info().recording_error){title_changed|=info_.station.title!=stream_->Info().title;info_.station=stream_->Info();changed=true;}
    source_rate_=info_.current.rate;
    int source=info_.current.channels;if(source<1||source>8||source_rate_<1000||source_rate_>768000){error_=L"The stream changed to an unsupported format.";return -1;}
    if(negotiate_){channels_=OutputChannels(settings_.channels,source);rate_=settings_.rate?settings_.rate:source_rate_;}
    if(gain_link_!=info_.current_link)Gain();if(title_changed)SetTitle(info_);if(changed)++metadata_revision_;
    const bool raw=settings_.channels==ChannelMode::LegacyUnmapped&&channels_==source;
    if(!reader_->WindowsOrder()&&!raw){
        if(source==6&&settings_.legacy_51_order)for(int f=0;f<n;++f)std::swap(scratch_[f*source+1],scratch_[f*source+2]);
        winamp_opus::ReorderToWindows(scratch_.data(),n,source);
    }
    auto mix=settings_.channels;
    if(OutputChannels(mix,source)!=channels_)mix=channels_==1?ChannelMode::Mono:channels_==2?ChannelMode::Stereo:channels_==4?ChannelMode::Quad:ChannelMode::Source;
    const bool identity = mix==ChannelMode::Source || mix==ChannelMode::LegacyUnmapped ||
        (mix==ChannelMode::Stereo&&source<=2) || (mix==ChannelMode::Mono&&source==1) ||
        (mix==ChannelMode::Quad&&source==4);
    if(const auto* exact=reader_->IntegerPcm();exact&&identity&&source==channels_&&gain_==1.){
        std::copy_n(exact,n*channels_,integer_.data());last_exact_=true;
    }
    for(int frame=0;frame<n;++frame)MixFrame(scratch_.data()+frame*source,source,out+frame*channels_,channels_,mix,gain_);
    return n;
}
void Audio::FallbackToStereo(float* pending,int frames){
    last_exact_=false;
    const int source=channels_;
    if(settings_.channels==ChannelMode::LegacyUnmapped&&!reader_->WindowsOrder()){
        if(source==6&&settings_.legacy_51_order)for(int f=0;f<frames;++f)std::swap(pending[f*source+1],pending[f*source+2]);
        winamp_opus::ReorderToWindows(pending,frames,source);
    }
    for(int f=0;f<frames;++f){float stereo[2]{};MixFrame(pending+f*source,source,stereo,2,ChannelMode::Stereo,1.);pending[f*2]=stereo[0];pending[f*2+1]=stereo[1];}
    channels_=2;settings_.channels=ChannelMode::Stereo;
}
void Audio::Cancel()noexcept{cancelled_=true;std::shared_ptr<ByteStream> source;{std::lock_guard<std::mutex> guard(stream_mutex_);source=stream_;}if(source)source->Cancel();}
bool Audio::Seek(int ms){last_exact_=false;return reader_&&reader_->Seek(ms);}
Pcm::Pcm(Audio& a,Settings s):audio_(a),settings_(Validate(s)),rate_(a.Rate()),channels_(a.Channels()){
    quantizer_=std::make_unique<Quantizer>(settings_,channels_,rate_);
    ready_=SetRate(audio_.SourceRate());
}
Pcm::~Pcm(){if(resampler_)speex_resampler_destroy(resampler_);}
bool Pcm::SetRate(int rate){
    if(rate==source_rate_)return true;
    int error=0;
    if(!resampler_&&rate!=rate_){
        resampler_=speex_resampler_init(channels_,rate,rate_,settings_.quality,&error);
        if(!resampler_||error)return false;
        speex_resampler_skip_zeros(resampler_);
    }else if(resampler_&&speex_resampler_set_rate(resampler_,rate,rate_))return false;
    source_rate_=rate;return true;
}
void Pcm::FinishSegment()noexcept{
    output_target_=static_cast<std::int64_t>(expected_+1e-9L);
    padding_=resampler_?static_cast<int>(speex_resampler_get_input_latency(resampler_))+BlockFrames:0;
}
int Pcm::Render(int capacity){
    output_exact_.fill(false);
    if(pending_error_)return std::exchange(pending_error_,0);
    if(!ready_||capacity<1||capacity>BlockFrames)return -1;
    int made=0;
    while(made<capacity){
        if(input_offset_>=input_frames_&&!eof_&&!format_pending_){
            input_frames_=audio_.Read(input_.data(),BlockFrames);input_offset_=0;input_exact_=false;
            if(input_frames_>0&&settings_.exact_flac&&settings_.bits==32&&!settings_.dither){
                if(const auto* exact=audio_.IntegerPcm()){
                    std::copy_n(exact,input_frames_*audio_.Channels(),integer_input_.data());input_exact_=true;
                }
            }
            if(input_frames_<0){pending_error_=input_frames_;input_frames_=0;break;}
            if(!input_frames_){eof_=true;FinishSegment();}
            else if(audio_.Channels()!=channels_||audio_.Rate()!=rate_){
                // Retain the entire first new-format block. It must not pass
                // through the old resampler or be packed using the old stride.
                next_frames_=input_frames_;next_exact_=input_exact_;
                if(next_exact_)std::copy_n(integer_input_.data(),next_frames_*audio_.Channels(),next_integer_.data());
                std::copy_n(input_.data(),next_frames_*audio_.Channels(),next_input_.data());
                input_frames_=0;format_pending_=true;FinishSegment();
            }else{
                if(!SetRate(audio_.SourceRate()))return -1;
                expected_+=static_cast<long double>(input_frames_)*rate_/source_rate_;
            }
        }
        if((eof_||format_pending_)&&emitted_+made>=output_target_)break;
        if(input_offset_>=input_frames_&&(eof_||format_pending_)){
            if(!resampler_||padding_<=0)break;
            input_.fill(0);input_exact_=false;input_offset_=0;input_frames_=std::min(BlockFrames,padding_);padding_-=input_frames_;
        }
        if(!resampler_){
            const int n=std::min(capacity-made,input_frames_-input_offset_);if(n<=0)break;
            std::copy_n(input_.data()+input_offset_*channels_,n*channels_,output_.data()+made*channels_);
            if(input_exact_){
                std::copy_n(integer_input_.data()+input_offset_*channels_,n*channels_,integer_output_.data()+made*channels_);
                std::fill_n(output_exact_.data()+made,n,true);
            }
            input_offset_+=n;made+=n;
        }else{
            spx_uint32_t in=input_frames_-input_offset_,out=capacity-made;
            if(eof_||format_pending_)out=static_cast<spx_uint32_t>(std::min<std::int64_t>(out,output_target_-emitted_-made));
            const auto old=in;
            const int result=speex_resampler_process_interleaved_float(resampler_,input_.data()+input_offset_*channels_,&in,output_.data()+made*channels_,&out);
            if(result)return made?made:-1;
            input_offset_+=in;made+=out;if(!in&&!out&&old)return -1;
        }
    }
    emitted_+=made;
    if(!made&&pending_error_)return std::exchange(pending_error_,0);
    if(!made&&format_pending_)return FormatChange;
    return made;
}
int Pcm::Read(void* pcm,int capacity){
    if(!pcm)return -1;const int made=Render(capacity);
    if(made>0){
        quantizer_->Pack(pcm,output_.data(),made);
        // Winamp is little-endian x86. Restore eligible PCM directly, including
        // INT32_MIN/MAX and low bits that cannot survive a float32 round-trip.
        if(settings_.bits==32)for(int frame=0;frame<made;++frame)if(output_exact_[frame])
            std::memcpy(static_cast<unsigned char*>(pcm)+frame*channels_*4,
                        integer_output_.data()+frame*channels_,channels_*4);
    }
    return made;
}
int Pcm::ReadFloat(float* pcm,int capacity){if(!pcm)return -1;const int made=Render(capacity);if(made>0)std::copy_n(output_.data(),made*channels_,pcm);return made;}
bool Pcm::AcceptFormat(bool stereo_fallback){
    if(!format_pending_||emitted_<output_target_)return false;
    if(stereo_fallback){audio_.FallbackToStereo(next_input_.data(),next_frames_);next_exact_=false;}
    base_ms_+=expected_*1000/rate_;
    if(resampler_){speex_resampler_destroy(resampler_);resampler_=nullptr;}
    rate_=audio_.Rate();channels_=audio_.Channels();source_rate_=0;
    quantizer_=std::make_unique<Quantizer>(settings_,channels_,rate_);
    ready_=SetRate(audio_.SourceRate());if(!ready_)return false;
    std::copy_n(next_input_.data(),next_frames_*channels_,input_.data());
    input_exact_=next_exact_;
    if(input_exact_)std::copy_n(next_integer_.data(),next_frames_*channels_,integer_input_.data());
    next_exact_=false;
    input_frames_=next_frames_;input_offset_=padding_=next_frames_=pending_error_=0;
    emitted_=output_target_=0;expected_=static_cast<long double>(input_frames_)*rate_/source_rate_;
    eof_=format_pending_=false;return true;
}
void Pcm::Reset(int ms)noexcept{
    quantizer_->Reset();
    input_exact_=next_exact_=false;output_exact_.fill(false);
    if(resampler_){speex_resampler_reset_mem(resampler_);speex_resampler_skip_zeros(resampler_);}
    input_frames_=input_offset_=padding_=pending_error_=next_frames_=0;
    emitted_=output_target_=0;expected_=0;eof_=format_pending_=false;base_ms_=std::max(0,ms);
}
int Pcm::PositionMs()const noexcept{
    return static_cast<int>(std::min<long double>(INT_MAX,base_ms_+static_cast<long double>(emitted_)*1000/rate_));
}
}
