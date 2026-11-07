#include "internal.hpp"
#include <limits>
#include <cstddef>
using namespace och::core;
namespace {
thread_local std::string last_error;
template<class F> int protect(F f) noexcept {
    try{return f();}catch(const Failure& e){last_error=e.what();return e.code;}
    catch(const std::exception& e){last_error=e.what();return OCH_IO;}
    catch(...){last_error="unexpected internal error";return OCH_IO;}
}
template<class T> void out_struct(T* p){require(p && p->struct_size==sizeof(T),OCH_INVALID,"output struct_size mismatch");}
// Structures extended in 0.6.0 accept the earlier, shorter layout too.
constexpr size_t meta_v1=offsetof(och_metadata,time_ticks), event_v1=offsetof(och_event,hw_time);
template<class T> void out_struct_v(T* p,size_t v1){require(p && (p->struct_size==sizeof(T)||p->struct_size==v1),OCH_INVALID,"output struct_size mismatch");}
void put_meta(och_metadata* out,const och_metadata& m){const auto n=out->struct_size;memcpy(out,&m,n);out->struct_size=n;}
void device(och_device* p){require(p!=nullptr,OCH_INVALID,"null device");}
void stream(och_stream* s,unsigned direction){require(s && s->args.direction==direction,OCH_INVALID,"wrong or null stream direction");}
och_metadata adjusted(och_stream* s,const och::core::Ring::Block& b) {
    auto m=b.meta;m.sample_index+=s->recv_offset/s->group;
    if(s->recv_offset){
        const bool timed=(m.flags&OCH_META_HAS_TIME)!=0;
        m.flags=timed?uint32_t(OCH_META_HAS_TIME|(m.flags&OCH_META_END_OF_BURST)):uint32_t(OCH_META_TIME_UNKNOWN);
        m.host_dropped_samples=0;m.device_dropped_samples=0;
        if(timed)m.time_ticks+=s->recv_offset/s->group;
    }
    return m;
}
int await_rx(och_stream* s,uint32_t timeout) {
    if(s->reconfiguring)return OCH_TIMEOUT;
    if(s->ring.read())return OCH_OK;
    if(!s->running){auto e=s->error();if(!e.empty())throw Failure(OCH_IO,e);return OCH_STOPPED;}
    const bool ready=s->ring.wait_read(timeout,s->stop_requested,&s->reconfiguring);
    if(s->reconfiguring)return OCH_TIMEOUT;
    if(ready)return OCH_OK;
    if(!s->running){auto e=s->error();if(!e.empty())throw Failure(OCH_IO,e);return OCH_STOPPED;}
    return OCH_TIMEOUT;
}
}
extern "C" {
int och_get_power_status(och_device* d,och_power_status* p,uint32_t timeout){return protect([&]{device(d);out_struct(p);std::lock_guard<std::mutex> lock(d->state_mutex);*p=d->get_power_status(timeout);return OCH_OK;});}
int och_get_fir(och_device* d,och_direction dir,och_fir_config* c){return protect([&]{device(d);out_struct(c);std::lock_guard<std::mutex> lock(d->state_mutex);*c=d->get_fir(dir);return OCH_OK;});}
int och_set_fir(och_device* d,const och_fir_config* c){return protect([&]{device(d);require(c!=nullptr,OCH_INVALID,"null FIR config");std::lock_guard<std::mutex> lock(d->state_mutex);d->set_fir(*c);return OCH_OK;});}
int och_set_fir_enabled(och_device* d,uint32_t r,uint32_t t){return protect([&]{device(d);std::lock_guard<std::mutex> lock(d->state_mutex);d->enable_fir(r,t);return OCH_OK;});}
int och_get_clock(och_device* d,och_clock_config* c){return protect([&]{device(d);out_struct(c);std::lock_guard<std::mutex> lock(d->state_mutex);*c=d->get_clock();return OCH_OK;});}
int och_set_clock(och_device* d,const och_clock_config* c){return protect([&]{device(d);require(c!=nullptr,OCH_INVALID,"null clock config");std::lock_guard<std::mutex> lock(d->state_mutex);d->set_clock(*c);return OCH_OK;});}
int och_set_sample_rate(och_device* d,uint32_t rate){return protect([&]{device(d);std::lock_guard<std::mutex> lock(d->state_mutex);d->set_sample_rate(rate);return OCH_OK;});}
int och_get_rx_correction(och_device* d,och_rx_correction_config* c){return protect([&]{device(d);out_struct(c);std::lock_guard<std::mutex> lock(d->state_mutex);*c=d->get_rx_correction();return OCH_OK;});}
int och_set_rx_dc_correction(och_device* d,uint32_t enabled){return protect([&]{device(d);std::lock_guard<std::mutex> lock(d->state_mutex);d->set_rx_correction(true,enabled);return OCH_OK;});}
int och_set_rx_iq_correction(och_device* d,uint32_t enabled){return protect([&]{device(d);std::lock_guard<std::mutex> lock(d->state_mutex);d->set_rx_correction(false,enabled);return OCH_OK;});}
int och_get_lo_source(och_device* d,och_lo_source_config* c){return protect([&]{device(d);out_struct(c);std::lock_guard<std::mutex> lock(d->state_mutex);*c=d->get_lo_source();return OCH_OK;});}
int och_set_rx_lo_source(och_device* d,uint32_t source){return protect([&]{device(d);std::lock_guard<std::mutex> lock(d->state_mutex);d->set_lo_source(true,source);return OCH_OK;});}
int och_set_tx_lo_source(och_device* d,uint32_t source){return protect([&]{device(d);std::lock_guard<std::mutex> lock(d->state_mutex);d->set_lo_source(false,source);return OCH_OK;});}
int och_get_reference_status(och_device* d,och_reference_status* c){return protect([&]{device(d);out_struct(c);std::lock_guard<std::mutex> lock(d->state_mutex);*c=d->get_reference_status();return OCH_OK;});}
int och_set_reference_source(och_device* d,uint32_t source){return protect([&]{device(d);std::lock_guard<std::mutex> lock(d->state_mutex);d->set_reference_source(source);return OCH_OK;});}
int och_test_reference(och_device* d,och_reference_test_result* c){return protect([&]{device(d);out_struct(c);std::lock_guard<std::mutex> lock(d->state_mutex);*c=d->test_reference();return OCH_OK;});}
int och_get_pps_status(och_device* d,och_pps_status* c){return protect([&]{device(d);out_struct(c);std::lock_guard<std::mutex> lock(d->state_mutex);*c=d->get_pps_status();return OCH_OK;});}
int och_set_pps_enabled(och_device* d,uint32_t enabled){return protect([&]{device(d);std::lock_guard<std::mutex> lock(d->state_mutex);d->set_pps_enabled(enabled);return OCH_OK;});}
int och_get_vcxo_status(och_device* d,och_vcxo_status* c){return protect([&]{device(d);out_struct(c);std::lock_guard<std::mutex> lock(d->state_mutex);*c=d->get_vcxo_status();return OCH_OK;});}
int och_get_rgb_status(och_device* d,och_rgb_status* c){return protect([&]{device(d);out_struct(c);std::lock_guard<std::mutex> lock(d->state_mutex);*c=d->get_rgb_status();return OCH_OK;});}
int och_get_fan_status(och_device* d,och_fan_status* c){return protect([&]{device(d);out_struct(c);std::lock_guard<std::mutex> lock(d->state_mutex);*c=d->get_fan_status();return OCH_OK;});}
int och_get_rx_rssi(och_device* d,och_rx_rssi* c){return protect([&]{device(d);out_struct(c);std::lock_guard<std::mutex> lock(d->state_mutex);*c=d->get_rx_rssi();return OCH_OK;});}
int och_set_rx_rssi_enabled(och_device* d,uint32_t enabled){return protect([&]{device(d);std::lock_guard<std::mutex> lock(d->state_mutex);d->set_rx_rssi_enabled(enabled);return OCH_OK;});}
int och_get_core_temperature(och_device* d,och_core_temperature* c){return protect([&]{device(d);out_struct(c);std::lock_guard<std::mutex> lock(d->state_mutex);*c=d->get_core_temperature();return OCH_OK;});}
int och_set_rgb_enabled(och_device* d,uint32_t enabled){return protect([&]{device(d);std::lock_guard<std::mutex> lock(d->state_mutex);d->set_rgb_enabled(enabled);return OCH_OK;});}
int och_set_fan_enabled(och_device* d,uint32_t enabled){return protect([&]{device(d);std::lock_guard<std::mutex> lock(d->state_mutex);d->set_fan_enabled(enabled);return OCH_OK;});}
int och_firmware_query(const char* serial,och_firmware_info* p){return protect([&]{out_struct(p);*p=firmware_query(serial);return OCH_OK;});}
int och_get_firmware_info(och_device* d,och_firmware_info* p){return protect([&]{device(d);out_struct(p);*p=d->firmware;return OCH_OK;});}
int och_firmware_read_image(const char* path,och_firmware_image* p){return protect([&]{out_struct(p);*p=firmware_read_image(path);return OCH_OK;});}
int och_firmware_update(const char* serial,const char* path,och_firmware_progress progress,void* user){return protect([&]{firmware_update(serial,path,progress,user);return OCH_OK;});}
int och_firmware_enter_bootloader(const char* serial){return protect([&]{firmware_enter_bootloader(serial);return OCH_OK;});}
int och_firmware_start_app(const char* serial){return protect([&]{firmware_start_app(serial);return OCH_OK;});}
const char* och_version(){return OCH_VERSION;}
const char* och_last_error(){return last_error.c_str();}
void och_default_config(och_config* c){if(!c)return;*c={};c->struct_size=sizeof(*c);c->sample_rate=30720000;c->rx_mask=3;c->tx_mask=0;c->rx_frequency=c->tx_frequency=1000000000ULL;c->rx_bandwidth=c->tx_bandwidth=18000000;c->rx_gain[0]=c->rx_gain[1]=30;c->tx_attenuation[0]=c->tx_attenuation[1]=10;c->rx_agc=2;}
void och_default_stream_args(och_stream_args* a,och_direction d){if(!a)return;*a={};a->struct_size=sizeof(*a);a->direction=d;a->format=OCH_SC16;a->transfer_bytes=196608;a->usb_queue_depth=32;a->host_buffer_bytes=64U*1024*1024;}
size_t och_sample_size(och_format f){return f==OCH_PACKED12?3:f==OCH_SC16?4:f==OCH_FC32?8:f==OCH_SC8?2:0;}
int och_list(och_device_info* output,size_t capacity,size_t* count){return protect([&]{
    require(count && (!capacity||output),OCH_INVALID,"invalid device list output");
    auto all=och::transport::scanDevices(0x1a86,0x5537);*count=all.size();
    for(size_t i=0;i<std::min(capacity,all.size());++i)out_struct(&output[i]);
    for(size_t i=0;i<std::min(capacity,all.size());++i)fill_info(output[i],all[i]);
    return OCH_OK;
});}
int och_open(const char* serial,och_device** out){return protect([&]{
    require(out!=nullptr,OCH_INVALID,"null device output");*out=nullptr;
    auto all=och::transport::scanDevices(0x1a86,0x5537);std::vector<och::transport::DeviceInfo> match;
    for(auto& d:all)if(!serial||!*serial||serial_of(d)==serial)match.push_back(d);
#ifdef _WIN32
    require(!match.empty(),OCH_NOT_FOUND,"OmniCoreSDR with libusbK not found; run och-driver check");
#else
    require(!match.empty(),OCH_NOT_FOUND,"OmniCoreSDR USB device not found (VID 1a86, PID 5537)");
#endif
    require(match.size()==1,OCH_INVALID,"multiple devices; specify a serial number");
    auto d=std::make_unique<och_device>();d->open(match.front());*out=d.release();return OCH_OK;
});}
int och_close(och_device* d){return protect([&]{device(d);{std::lock_guard<std::mutex> lock(d->state_mutex);require(!d->rx&&!d->tx,OCH_BUSY,"destroy RX/TX streams before closing device");}delete d;return OCH_OK;});}
int och_get_info(och_device* d,och_device_info* i){return protect([&]{device(d);out_struct(i);*i=d->info;return OCH_OK;});}
int och_configure(och_device* d,const och_config* c){return protect([&]{device(d);require(c!=nullptr,OCH_INVALID,"null config");std::lock_guard<std::mutex> lock(d->state_mutex);d->configure(*c);return OCH_OK;});}
int och_get_config(och_device* d,och_config* c){return protect([&]{device(d);out_struct(c);std::lock_guard<std::mutex> lock(d->state_mutex);require(d->configured,OCH_INVALID,"device has not been configured by this handle");*c=d->config;return OCH_OK;});}
int och_command(och_device* d,const char* cmd,char* reply,size_t capacity,uint32_t timeout){return protect([&]{
    device(d);require(cmd&&reply&&capacity,OCH_INVALID,"invalid command/reply buffer");reply[0]=0;
    std::lock_guard<std::mutex> lock(d->state_mutex);
    std::string text(cmd);bool query=!text.empty()&&text.back()=='?' && text.find('=')==std::string::npos;
    require(query||(!d->rx&&!d->tx),OCH_BUSY,"raw mutations require no streams; use device configure");
    if(!query){d->configured=false;d->lo_sources=-1;} // A failed firmware command may have applied partially.
    auto result=d->command(text,timeout);copy_text(reply,capacity,result);
    require(result.size()<capacity,OCH_BUFFER_SMALL,"command reply buffer too small (reply truncated)");return OCH_OK;
});}
int och_poll_event(och_device* d,och_event* e,uint32_t timeout){return protect([&]{
    device(d);out_struct_v(e,event_v1);std::unique_lock<std::mutex> lock(d->events_mutex);
    if(!d->events_cv.wait_for(lock,std::chrono::milliseconds(timeout),[&]{return !d->events.empty();}))return OCH_TIMEOUT;
    const auto n=e->struct_size;memcpy(e,&d->events.front(),n);e->struct_size=n;d->events.pop_front();return OCH_OK;
});}
int och_create_stream(och_device* d,const och_stream_args* a,och_stream** out){return protect([&]{
    device(d);require(a&&out,OCH_INVALID,"null stream arguments");*out=nullptr;
    require(a->struct_size==sizeof(*a) && a->direction<=OCH_TX && a->format<=OCH_SC8,OCH_INVALID,"stream ABI/direction/format invalid");
    require(a->transfer_bytes>=16384 && a->transfer_bytes<=1048576 && a->transfer_bytes%16384==0,OCH_INVALID,"transfer_bytes must be a multiple of 16384 in 16384..1048576");
    require(a->usb_queue_depth>=2 && a->usb_queue_depth<=128,OCH_INVALID,"USB queue depth must be 2..128");
    require(a->host_buffer_bytes>=a->transfer_bytes*4ULL && a->host_buffer_bytes<=512U*1024*1024,OCH_INVALID,"host buffer must hold at least four blocks and at most 512 MiB");
    std::lock_guard<std::mutex> lock(d->state_mutex);require(d->configured,OCH_INVALID,"configure before creating streams");
    require(a->format!=OCH_SC8 || iq_bits(d->config)==8,OCH_INVALID,"sc8 application format requires 8-bit USB IQ mode");
    auto& slot=a->direction==OCH_RX?d->rx:d->tx;require(slot==nullptr,OCH_BUSY,"only one stream per direction");
    require((a->direction==OCH_RX?d->config.rx_mask:d->config.tx_mask)!=0,OCH_INVALID,"stream direction has no enabled channels");
    slot=new och_stream(d,*a);*out=slot;return OCH_OK;
});}
int och_destroy_stream(och_stream* s){return protect([&]{
    require(s!=nullptr,OCH_INVALID,"null stream");auto d=s->dev;std::unique_lock<std::mutex> lock(d->state_mutex);
    require(!s->lease,OCH_BUSY,"release RX lease before destroying stream");
    std::string issue;try{s->stop(0);}catch(const std::exception& e){issue=e.what();}
    if(s->args.direction==OCH_RX)d->rx=nullptr;else d->tx=nullptr;
    delete s;if(!issue.empty())throw Failure(OCH_IO,issue);return OCH_OK;
});}
int och_start(och_stream* s){return protect([&]{require(s!=nullptr,OCH_INVALID,"null stream");std::lock_guard<std::mutex> lock(s->dev->state_mutex);s->start();return OCH_OK;});}
int och_stop(och_stream* s,uint32_t timeout){return protect([&]{require(s!=nullptr,OCH_INVALID,"null stream");std::lock_guard<std::mutex> lock(s->dev->state_mutex);s->stop(timeout);return OCH_OK;});}
int och_flush(och_stream* s,uint32_t timeout){return protect([&]{stream(s,OCH_TX);std::unique_lock<std::mutex> io(s->io_mutex,std::try_to_lock);require(io.owns_lock(),OCH_BUSY,"TX IO in progress");s->flush(timeout);return OCH_OK;});}
int och_recv(och_stream* s,void* output,size_t capacity,size_t* received,och_metadata* m,uint32_t timeout){return protect([&]{
    stream(s,OCH_RX);require(output&&capacity&&received,OCH_INVALID,"invalid receive buffer");*received=0;out_struct_v(m,meta_v1);
    std::unique_lock<std::mutex> io(s->io_mutex,std::try_to_lock);if(!io.owns_lock() || s->reconfiguring)return OCH_TIMEOUT;
    require(!s->lease,OCH_BUSY,"release zero-copy lease before recv");
    int rc=await_rx(s,timeout);if(rc)return static_cast<och_result>(rc);
    auto b=s->ring.read();size_t frames=std::min(capacity,(b->size-s->recv_offset)/s->group);auto p=b->data.get()+s->recv_offset;put_meta(m,adjusted(s,*b));
    if(s->args.format==OCH_SC8)memcpy(output,p,frames*s->group);
    else if(s->args.format==OCH_PACKED12){if(s->wire_bytes==2)och::dsp::unpack8_packed12(p,static_cast<uint8_t*>(output),frames*s->nchan);else memcpy(output,p,frames*s->group);}
    else if(s->args.format==OCH_SC16){require(reinterpret_cast<uintptr_t>(output)%alignof(int16_t)==0,OCH_INVALID,"sc16 buffer alignment");
        (s->wire_bytes==2?och::dsp::unpack8_sc16:och::dsp::unpack_sc16)(p,static_cast<int16_t*>(output),frames*s->nchan);}
    else {require(reinterpret_cast<uintptr_t>(output)%alignof(float)==0,OCH_INVALID,"fc32 buffer alignment");
        (s->wire_bytes==2?och::dsp::unpack8_fc32:och::dsp::unpack_fc32)(p,static_cast<float*>(output),frames*s->nchan);}
    s->recv_offset+=frames*s->group;s->samples+=frames;*received=frames;
    if(s->recv_offset==b->size){s->ring.release();s->recv_offset=0;}return OCH_OK;
});}
int och_rx_acquire(och_stream* s,const void** data,size_t* bytes,och_metadata* m,uint32_t timeout){return protect([&]{
    stream(s,OCH_RX);require(data&&bytes,OCH_INVALID,"null lease output");*data=nullptr;*bytes=0;out_struct_v(m,meta_v1);
    std::unique_lock<std::mutex> io(s->io_mutex,std::try_to_lock);if(!io.owns_lock() || s->reconfiguring)return OCH_TIMEOUT;
    require(!s->lease,OCH_BUSY,"only one RX lease allowed");
    int rc=await_rx(s,timeout);if(rc)return static_cast<och_result>(rc);
    auto b=s->ring.read();*data=b->data.get()+s->recv_offset;*bytes=b->size-s->recv_offset;put_meta(m,adjusted(s,*b));s->lease=true;return OCH_OK;
});}
int och_rx_release(och_stream* s){return protect([&]{stream(s,OCH_RX);std::lock_guard<std::mutex> io(s->io_mutex);require(s->lease,OCH_INVALID,"no RX lease");auto b=s->ring.read();s->samples+=(b->size-s->recv_offset)/s->group;s->ring.release();s->recv_offset=0;s->lease=false;return OCH_OK;});}
namespace {
int send_impl(och_stream* s,const void* data,size_t count,const och_tx_metadata* md,size_t* accepted,uint32_t timeout){return protect([&]{
    stream(s,OCH_TX);require(accepted && (data||!count),OCH_INVALID,"invalid TX input");*accepted=0;
    uint32_t mflags=0;uint64_t mtime=0;
    if(md){
        require(md->struct_size==sizeof(*md) && !(md->flags&~uint32_t(OCH_TX_HAS_TIME|OCH_TX_START_OF_BURST|OCH_TX_END_OF_BURST)),OCH_INVALID,"invalid och_tx_metadata");
        mflags=md->flags;mtime=md->time_ticks;
    }
    std::unique_lock<std::mutex> io(s->io_mutex,std::try_to_lock);if(!io.owns_lock() || s->reconfiguring)return OCH_TIMEOUT;
    require(count<=std::numeric_limits<size_t>::max()/(8*s->nchan),OCH_INVALID,"sample count overflow");
    require(!s->flushing,OCH_BUSY,"TX flush in progress");if(s->stop_requested)return OCH_STOPPED;
    size_t width=och_sample_size(static_cast<och_format>(s->args.format));auto p=static_cast<const uint8_t*>(data);
    if(s->args.format==OCH_SC16 || s->args.format==OCH_FC32)require(reinterpret_cast<uintptr_t>(data)%(s->args.format==OCH_SC16?alignof(int16_t):alignof(float))==0,OCH_INVALID,"TX sample buffer alignment");
    auto until=now_ns()+uint64_t(timeout)*1000000;
    while(*accepted<count){
        if(s->reconfiguring)return *accepted?OCH_OK:OCH_TIMEOUT;
        if(s->stop_requested)return *accepted?OCH_OK:OCH_STOPPED;
        auto b=s->ring.write();if(!b){
            if(s->ever_started&&!s->running)throw Failure(OCH_IO,"TX worker stopped: "+s->error());
            auto now=now_ns();if(now>=until)return *accepted?OCH_OK:OCH_TIMEOUT;
            auto remaining=static_cast<uint32_t>(std::max<uint64_t>(1,(until-now)/1000000));
            s->ring.wait_write(remaining,s->stop_requested,&s->reconfiguring);
            if(s->reconfiguring)return *accepted?OCH_OK:OCH_TIMEOUT;
            if(s->stop_requested)return *accepted?OCH_OK:OCH_STOPPED;
            continue;
        }
        size_t frames=std::min(count-*accepted,s->ring.block_bytes()/s->group);auto src=p+*accepted*width*s->nchan;
        if(s->args.format==OCH_SC8)memcpy(b->data.get(),src,frames*s->group);
        else if(s->args.format==OCH_PACKED12){if(s->wire_bytes==2)och::dsp::pack8_packed12(src,b->data.get(),frames*s->nchan);else memcpy(b->data.get(),src,frames*s->group);}
        else if(s->args.format==OCH_SC16)(s->wire_bytes==2?och::dsp::pack8_sc16:och::dsp::pack_sc16)(reinterpret_cast<const int16_t*>(src),b->data.get(),frames*s->nchan);
        else (s->wire_bytes==2?och::dsp::pack8_fc32:och::dsp::pack_fc32)(reinterpret_cast<const float*>(src),b->data.get(),frames*s->nchan);
        och_metadata tag{};
        if(*accepted==0){tag.flags=mflags&(OCH_TX_HAS_TIME|OCH_TX_START_OF_BURST);tag.time_ticks=mtime;}
        if(*accepted+frames==count)tag.flags|=mflags&OCH_TX_END_OF_BURST;
        s->ring.publish(frames*s->group,tag);*accepted+=frames;s->samples+=frames;
        auto used=s->ring.queued();if(used>s->peak)s->peak=used;
    }
    return OCH_OK;
});}
}
int och_send(och_stream* s,const void* data,size_t count,size_t* accepted,uint32_t timeout){return send_impl(s,data,count,nullptr,accepted,timeout);}
int och_send_burst(och_stream* s,const void* data,size_t count,const och_tx_metadata* md,size_t* accepted,uint32_t timeout){
    return send_impl(s,data,count,md,accepted,timeout);
}
int och_get_time(och_device* d,och_time_status* t){return protect([&]{
    device(d);out_struct(t);std::lock_guard<std::mutex> lock(d->state_mutex);*t=d->get_time();return OCH_OK;});}
int och_set_time(och_device* d,uint64_t ticks,uint32_t at_next_pps){return protect([&]{
    device(d);require(at_next_pps<=1,OCH_INVALID,"at_next_pps must be 0 or 1");
    std::lock_guard<std::mutex> lock(d->state_mutex);d->require_capability("hw_time");
    d->command((at_next_pps?"time_next_pps=":"time_now=")+std::to_string(ticks));return OCH_OK;});}
int och_set_time_source(och_device* d,uint32_t source){return protect([&]{
    device(d);require(source<=OCH_TIME_SOURCE_INTERNAL,OCH_INVALID,"time source must be 0..2");
    std::lock_guard<std::mutex> lock(d->state_mutex);d->require_capability("hw_time");
    static const char* names[]={"none","external","internal"};
    d->command(std::string("time_source=")+names[source]);return OCH_OK;});}
int och_rx_command(och_stream* s,uint32_t flags,uint64_t time,uint32_t num_samples){return protect([&]{
    stream(s,OCH_RX);
    require((s->args.start_flags&OCH_RX_TIMED)!=0,OCH_INVALID,"och_rx_command requires an RX stream created with OCH_RX_TIMED");
    require(!(flags&~uint32_t(OCH_RX_NOW|OCH_RX_STOP)),OCH_INVALID,"invalid RX command flags");
    std::lock_guard<std::mutex> lock(s->dev->state_mutex);
    require(s->started,OCH_INVALID,"start the RX stream before issuing commands");
    if(flags&OCH_RX_STOP)s->dev->command("rx_stream_stop!");
    else s->dev->command("rx_stream="+((flags&OCH_RX_NOW)?std::string("now"):std::to_string(time))+" "+std::to_string(num_samples));
    return OCH_OK;});}
int och_get_stats(och_stream* s,och_stats* st){return protect([&]{
    require(s!=nullptr,OCH_INVALID,"null stream");out_struct(st);*st={};st->struct_size=sizeof(*st);st->running=s->running;
    st->usb_bytes=s->usb_bytes;st->samples=s->samples;st->host_dropped_samples=s->host_dropped;st->headers=s->headers;st->resyncs=s->resyncs;
    st->host_queue_bytes=s->ring.queued();st->host_queue_peak_bytes=s->peak;st->usb_queue_pending=s->pending;st->transfer_errors=s->errors;
    st->device_overflows=s->dev->overflows;st->device_underflows=s->dev->underflows;st->event_drops=s->dev->event_drops;st->tx_starvation_events=s->starved;
    auto began=s->started_ns.load();auto end=s->running?now_ns():s->ended_ns.load();
    st->elapsed_ns=began && end>=began?end-began:0;st->tx_padding_samples=s->padding;return OCH_OK;
});}
}
