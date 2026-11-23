#include "codec_fixtures.hpp"
#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>
#include <FLAC/stream_encoder.h>
#include <speex/speex.h>
#include <speex/speex_header.h>
#include <array>
#include <cmath>
#include <fstream>
#include <vector>
namespace {
void Page(std::ofstream& out,ogg_page& page){out.write(reinterpret_cast<char*>(page.header),page.header_len);out.write(reinterpret_cast<char*>(page.body),page.body_len);Check(out.good(),"write codec fixture page");}
void Packet(std::ofstream& out,ogg_stream_state& stream,unsigned char* data,int size,int number,int granule,bool eos){ogg_packet p{};p.packet=data;p.bytes=size;p.packetno=number;p.granulepos=granule;p.b_o_s=number==0;p.e_o_s=eos;Check(!ogg_stream_packetin(&stream,&p),"packet input");ogg_page page{};while(ogg_stream_flush(&stream,&page))Page(out,page);}
}
void MakeVorbis(const std::filesystem::path& path,int channels,int rate,int frames,int serial){
    vorbis_info vi{};vorbis_info_init(&vi);Check(!vorbis_encode_init_vbr(&vi,channels,rate,.4f),"initialize Vorbis encoder");
    vorbis_comment vc{};vorbis_comment_init(&vc);vorbis_comment_add_tag(&vc,"TITLE","Vorbis fixture");vorbis_comment_add_tag(&vc,"ARTIST","Independent tests");
    vorbis_dsp_state dsp{};vorbis_block block{};Check(!vorbis_analysis_init(&dsp,&vi),"Vorbis analysis");Check(!vorbis_block_init(&dsp,&block),"Vorbis block");
    ogg_stream_state stream{};Check(!ogg_stream_init(&stream,serial),"Vorbis ogg");std::ofstream out(path,std::ios::binary);ogg_packet h{},c{},b{};Check(!vorbis_analysis_headerout(&dsp,&vc,&h,&c,&b),"Vorbis headers");
    ogg_stream_packetin(&stream,&h);ogg_stream_packetin(&stream,&c);ogg_stream_packetin(&stream,&b);ogg_page page{};while(ogg_stream_flush(&stream,&page))Page(out,page);
    int position=0;bool end=false;
    while(!end){int n=std::min(1024,frames-position);if(n){auto** pcm=vorbis_analysis_buffer(&dsp,n);for(int ch=0;ch<channels;++ch)for(int i=0;i<n;++i)pcm[ch][i]=static_cast<float>(.2*std::sin((position+i)*6.283185307179586*(220+ch*110)/rate));position+=n;}vorbis_analysis_wrote(&dsp,n);
        while(vorbis_analysis_blockout(&dsp,&block)==1){vorbis_analysis(&block,nullptr);vorbis_bitrate_addblock(&block);ogg_packet packet{};while(vorbis_bitrate_flushpacket(&dsp,&packet)){ogg_stream_packetin(&stream,&packet);while(ogg_stream_pageout(&stream,&page)){Page(out,page);if(ogg_page_eos(&page))end=true;}}}
        if(!n&&!end){while(ogg_stream_flush(&stream,&page)){Page(out,page);if(ogg_page_eos(&page))end=true;}}
    }
    ogg_stream_clear(&stream);vorbis_block_clear(&block);vorbis_dsp_clear(&dsp);vorbis_comment_clear(&vc);vorbis_info_clear(&vi);
}
void MakeFlac(const std::filesystem::path& path,int channels,int rate,int frames){
    auto* encoder=FLAC__stream_encoder_new();Check(encoder,"FLAC encoder");FLAC__stream_encoder_set_channels(encoder,channels);FLAC__stream_encoder_set_bits_per_sample(encoder,16);FLAC__stream_encoder_set_sample_rate(encoder,rate);FLAC__stream_encoder_set_total_samples_estimate(encoder,frames);
    FILE* file=nullptr;_wfopen_s(&file,path.c_str(),L"w+b");Check(file,"FLAC file");Check(FLAC__stream_encoder_init_ogg_FILE(encoder,file,nullptr,nullptr)==FLAC__STREAM_ENCODER_INIT_STATUS_OK,"FLAC Ogg init");
    std::vector<FLAC__int32> pcm(frames*channels);for(int i=0;i<frames;++i)for(int c=0;c<channels;++c)pcm[i*channels+c]=static_cast<FLAC__int32>(std::sin(i*6.283185307179586*(220+c*110)/rate)*8000);
    Check(FLAC__stream_encoder_process_interleaved(encoder,pcm.data(),frames),"FLAC encode");Check(FLAC__stream_encoder_finish(encoder),"FLAC finish");FLAC__stream_encoder_delete(encoder);
}
void MakeSpeex(const std::filesystem::path& path,int rate,int frames,int serial){
    const int mode=rate<=8000?0:rate<=16000?1:2;auto* state=speex_encoder_init(speex_lib_get_mode(mode));Check(state,"Speex encoder");int frame=0,lookahead=0;int quality=7;speex_encoder_ctl(state,SPEEX_SET_QUALITY,&quality);speex_encoder_ctl(state,SPEEX_GET_FRAME_SIZE,&frame);speex_encoder_ctl(state,SPEEX_GET_LOOKAHEAD,&lookahead);
    SpeexHeader header{};speex_init_header(&header,rate,1,speex_lib_get_mode(mode));header.frames_per_packet=1;int header_bytes=0;auto* bytes=speex_header_to_packet(&header,&header_bytes);
    std::ofstream out(path,std::ios::binary);ogg_stream_state stream{};ogg_stream_init(&stream,serial);Packet(out,stream,reinterpret_cast<unsigned char*>(bytes),header_bytes,0,0,false);speex_header_free(bytes);
    std::vector<unsigned char> comment{4,0,0,0,'t','e','s','t',0,0,0,0};Packet(out,stream,comment.data(),static_cast<int>(comment.size()),1,0,false);
    SpeexBits bits{};speex_bits_init(&bits);std::vector<spx_int16_t> pcm(frame);std::array<char,4096> packet{};int packets=(frames+lookahead+frame-1)/frame;
    for(int n=0;n<packets;++n){for(int i=0;i<frame;++i){auto pos=n*frame+i;pcm[i]=pos<frames?static_cast<spx_int16_t>(std::sin(pos*6.283185307179586*300/rate)*8000):0;}speex_bits_reset(&bits);Check(speex_encode_int(state,pcm.data(),&bits)>=0,"Speex encode");int count=speex_bits_write(&bits,packet.data(),static_cast<int>(packet.size()));bool last=n==packets-1;Packet(out,stream,reinterpret_cast<unsigned char*>(packet.data()),count,n+2,last?frames:((n+1)*frame-lookahead),last);}
    speex_bits_destroy(&bits);speex_encoder_destroy(state);ogg_stream_clear(&stream);
}
