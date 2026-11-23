// SPDX-License-Identifier: GPL-3.0-only
#include "backend.hpp"
#include <ogg/ogg.h>
#include <array>
#include <cstring>
namespace ogg_input {
Codec Probe(ByteStream& stream) noexcept {
    ogg_sync_state sync{}; if(ogg_sync_init(&sync))return Codec::Unknown;
    Codec found=Codec::Unknown; int total=0;
    while(total<65536&&found==Codec::Unknown){
        ogg_page page{};int status=ogg_sync_pageout(&sync,&page);
        if(status>0){
            if(!ogg_page_bos(&page))break;
            ogg_stream_state os{};
            if(ogg_stream_init(&os,ogg_page_serialno(&page)))break;
            if(!ogg_stream_pagein(&os,&page)){
                ogg_packet packet{};
                if(ogg_stream_packetout(&os,&packet)==1){
                    if(packet.bytes>=19&&!memcmp(packet.packet,"OpusHead",8))found=Codec::Opus;
                    else if(packet.bytes>=30&&packet.packet[0]==1&&!memcmp(packet.packet+1,"vorbis",6))found=Codec::Vorbis;
                    else if(packet.bytes>=9&&packet.packet[0]==127&&!memcmp(packet.packet+1,"FLAC",4))found=Codec::Flac;
                    else if(packet.bytes>=80&&!memcmp(packet.packet,"Speex   ",8))found=Codec::Speex;
                }
            }ogg_stream_clear(&os);continue;
        }
        if(status<0)continue;
        auto* dst=ogg_sync_buffer(&sync,512);if(!dst)break;
        int got=stream.Read(dst,512);if(got<=0)break;total+=got;ogg_sync_wrote(&sync,got);
    }
    ogg_sync_clear(&sync); if(!stream.RewindProbe())return Codec::Unknown;return found;
}
}
