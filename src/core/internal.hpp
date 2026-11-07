#pragma once
#include <omnicore/och.h>
#include "ring.hpp"
#include "power_status.hpp"
#include "../transport/transport.hpp"
#include "../dsp/codec.hpp"
#include "../dsp/frame.hpp"
#include <condition_variable>
#include <functional>
#include <future>
#include <deque>
#include <optional>
#include <cmath>

namespace och::core {
struct Failure:std::runtime_error {int code;Failure(int c,const std::string& s):std::runtime_error(s),code(c){}};
inline void require(bool ok,int code,const std::string& why){if(!ok)throw Failure(code,why);}
inline uint64_t now_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline uint32_t channels(uint32_t mask){return mask==3?2:1;}
inline uint32_t iq_bits(const och_config& c){return c.usb_iq_bits?c.usb_iq_bits:12;}
inline void copy_text(char* to,size_t n,const std::string& s){if(n){size_t k=std::min(n-1,s.size());memcpy(to,s.data(),k);to[k]=0;}}
template<class T,size_t N> class FixedQueue {
    std::array<T,N> values{};size_t first=0,count=0;
public:
    bool empty()const{return count==0;}size_t size()const{return count;}
    T front()const{return values[first];}
    void push_back(T p){if(count==N)throw std::logic_error("fixed USB queue overflow");values[(first+count)%N]=p;++count;}
    void pop_front(){first=(first+1)%N;--count;}
    void clear(){first=count=0;}
};
std::string serial_of(const och::transport::DeviceInfo&);
void fill_info(och_device_info&,const och::transport::DeviceInfo&);
// CH32 bridge firmware (firmware.cpp). firmware_probe is false for bridge
// firmware without the OCFW requests (legacy).
bool firmware_probe(och::transport::Usb&,och_firmware_info&,const std::string& serial);
och_firmware_image firmware_read_image(const char* path);
och_firmware_info firmware_query(const char* serial);
void firmware_enter_bootloader(const char* serial);
void firmware_start_app(const char* serial);
void firmware_update(const char* serial,const char* path,och_firmware_progress,void* user);
// EVT lines from the SDK time module; true when consumed (timebase.cpp).
bool timed_event(och_device&,const std::string& line);
// Multi-board sync (sync.cpp). Callers hold the device's state_mutex except
// for multi_sync, which takes each device's lock per transaction.
och_sync_status read_sync_status(och_device&);
void apply_sync_config(och_device&,const och_sync_config&);
void sync_trigger(och_device&,uint32_t mode);
void multi_sync(och_device* const* devices,size_t count,const och_multi_sync_args&,och_multi_sync_result* results);
}

struct och_device {
    och::transport::Usb usb;
    och::transport::Handle owner;
    bool owns_mutex=false;
    och_device_info info{};och_config config{};bool configured=false,transport_dirty=false;
    och_firmware_info firmware{}; // CH32 bridge, read at open
    std::mutex state_mutex;
    // state_mutex serializes control transactions; IO gates protect buffers
    // while a transaction reuses the existing public stream handles.
    bool reconfiguring_control=false;
    och_stream *rx=nullptr,*tx=nullptr;
    bool rx_on=false,tx_on=false;
    std::atomic<bool> control_stop{false};std::thread control_thread;
    std::mutex command_mutex, reply_mutex;
    std::condition_variable reply_cv;
    uint32_t next_id=0,pending_id=0;
    bool reply_started=false,reply_done=false,control_failed=false;int reply_result=0;
    std::string reply,control_error;
    std::mutex events_mutex;std::condition_variable events_cv;
    std::deque<och_event> events;
    std::atomic<uint64_t> event_seq{0},event_drops{0},overflows{0},underflows{0};
    och::core::PowerLogParser power_log;
    ~och_device();
    void open(const och::transport::DeviceInfo&);
    std::string command(const std::string&,uint32_t timeout=5000);
    void control_loop();void line(const std::string&);
    void emit(uint32_t type,const std::string&,uint64_t epoch=0,uint64_t hw_time=0,uint32_t code=0,uint32_t block=0,uint32_t count=1);
    void reset_transport();void configure(const och_config&);
    void apply_config(const och_config&);
    // LO, manual gain and attenuation changes that keep IQ streams running.
    void retune(const och_config&);
    bool tune_lo(const och_config&,const och_config& before,bool full);
    void tune_gain(const och_config&,const och_config& before,bool full,bool gain_table_changed);
    void reconfigure(const std::function<void()>&);
    och_fir_config get_fir(unsigned direction);
    void set_fir(const och_fir_config&);
    void enable_fir(unsigned rx_enabled,unsigned tx_enabled);
    och_clock_config get_clock();
    void set_clock(const och_clock_config&);void set_sample_rate(uint32_t rate);
    // Applies rate or, if the SDK has no exact clock plan for it, the nearest
    // accepted rate; returns the applied rate.
    uint32_t apply_sample_rate(uint32_t rate);
    uint32_t requested_sample_rate=0; // last request mapped to config.sample_rate
    och_rx_correction_config get_rx_correction();
    void set_rx_correction(bool dc,unsigned enabled);
    och_lo_source_config get_lo_source();
    void set_lo_source(bool receive,unsigned source);
    och_reference_status get_reference_status();
    void set_reference_source(unsigned source);
    och_reference_test_result test_reference();
    och_pps_status get_pps_status();
    void set_pps_enabled(unsigned enabled);
    och_vcxo_status get_vcxo_status();
    och_rgb_status get_rgb_status();
    void set_rgb_enabled(unsigned enabled);
    och_fan_status get_fan_status();
    void set_fan_enabled(unsigned enabled);
    och_core_temperature get_core_temperature();
    och_rx_rssi get_rx_rssi();
    void set_rx_rssi_enabled(unsigned enabled);
    // system_info? from open; capabilities are fixed for a firmware image.
    std::string capabilities;
    // Cached LO selection: bit0 RX external, bit1 TX external; -1 unknown.
    int lo_sources=-1;
    bool supports_capability(const char*);
    void require_capability(const char*);
    och_power_status get_power_status(uint32_t timeout_ms);
    och_time_status get_time();
};

struct och_stream {
    struct Slot {
        OVERLAPPED ov{};och::transport::Handle event;
        och::transport::TransferBuffer bytes;size_t used=0;bool pending=false;
        Slot(och::transport::Usb& usb,size_t n):event(CreateEventW(nullptr,TRUE,FALSE,nullptr)),bytes(usb,n){
            och::core::require(event.h!=nullptr,OCH_IO,"cannot create USB event");ov.hEvent=event.h;memset(bytes.get(),0,n);
        }
    };
    och_device* dev;och_stream_args args;unsigned nchan,wire_bytes,group;
    och::core::Ring ring;
    // Framed transport: 16-byte header per 16 KiB block (the only transport).
    och::dsp::FrameParser fparser;
    // Timed framed RX: one 16 KiB device block per USB transfer, so a finite
    // burst's last block (and its END_OF_BURST) completes without a short packet.
    size_t xfer_bytes=0;
    bool have_time=false;uint64_t next_time=0,cur_time=0,fr_epochs=0;int64_t gap_samples=0;
    bool blk_open=false,tx_first=true;size_t blk_start=0,blk_used=0;uint8_t blk_flags=0;uint64_t blk_time=0;uint16_t tx_seq=0;
    std::vector<std::unique_ptr<Slot>> slots;och::core::FixedQueue<Slot*,128> active;
    och::transport::Handle stop_event;
    std::thread worker;
    std::atomic<bool> stop_requested{false},running{false},queued{false},started{false},flushing{false};
    std::atomic<bool> ever_started{false};bool lease=false;size_t recv_offset=0;
    std::mutex io_mutex;
    std::atomic<bool> reconfiguring{false};
    std::mutex failure_mutex;std::string failure;
    std::atomic<uint64_t> usb_bytes{0},samples{0},host_dropped{0},headers{0},resyncs{0},peak{0},pending{0},errors{0},starved{0},padding{0};
    std::atomic<uint64_t> cancelled_bytes{0};
    std::atomic<size_t> assembling{0};
    std::atomic<uint64_t> started_ns{0},ended_ns{0};
    uint64_t epoch=0,epoch_base=0,index=0,dropped_pending=0,last_drop_notice=0;uint32_t flags_pending=0;
    bool reconfigured_rx=false;
    // Set after a running-stream retune; the next RX block starts a new segment.
    std::atomic<bool> retuned{false};
    // RX producer partial output block, always one epoch per block.
    size_t rx_filled=0;och_metadata rx_meta{};
    // TX consumer partial input block.
    size_t tx_offset=0;
    och_stream(och_device*,const och_stream_args&);
    ~och_stream();
    void start();void stop(uint32_t timeout_ms);void flush(uint32_t timeout_ms);void run();
    void halt();void prepare_restart(bool receive_gap);
    void fail(const std::string&);std::string error();
    bool submit(Slot*);void cancel_all();
    void publish_rx();
    void rx_frame(const och::dsp::FrameHeader&,const uint8_t*,size_t);
    static void frame_sink(void*,const och::dsp::FrameHeader&,const uint8_t*,size_t);
    bool fill_tx(Slot*);void tx_open(Slot*,uint32_t flags,uint64_t time);void tx_close(Slot*,uint8_t extra);
    size_t frame_groups()const{return wire_bytes==2?2046:1364;}
};
