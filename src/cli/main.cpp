// SPDX-License-Identifier: GPL-3.0-or-later
#include <omnicore/och.hpp>
#include "options.hpp"
#include "fir_file.hpp"
#include "commands.hpp"
#include "presentation.hpp"
#include "board_view.hpp"
#include "monitor_view.hpp"
#include "help_view.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#if defined(_WIN32) && !defined(OCH_CLI_TEST)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#elif defined(__linux__) && !defined(OCH_CLI_TEST)
#include <unistd.h>
#include <sys/ioctl.h>
#endif

using namespace och_cli;
using och_cli::help_view::print_help;
using och_cli::help_view::help_text;
using och_cli::help_view::print_catalog;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) { interrupted = 1; }
using clock_type = std::chrono::steady_clock;
std::string monitor_reply(och::device& device,const std::string& command,const std::string& raw) {
    if(!monitor_view::applies(command))return raw;
    try {
        const auto pps=device.pps_status();
        // Older SDKs have no vcxo_status?; keep the PPS-only display for them.
        och_vcxo_status vcxo{};bool has_vcxo=false;
        try {vcxo=device.vcxo_status();has_vcxo=true;} catch(const och::error&) {}
        return monitor_view::reply(raw,&pps,has_vcxo?&vcxo:nullptr);
    } catch(const och::error&) {
        // An optional monitor must not block external-reference selection.
        // Explicit PPS commands still surface monitoring errors to the caller.
        if(command.rfind("pps_",0)==0)throw;
        return monitor_view::reply(raw,nullptr);
    }
}
std::string display_number(double value, unsigned precision = 2) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}
// The core substitutes the nearest accepted rate when the SDK has no exact
// clock plan for the request; say so instead of silently changing the rate.
void report_rate_adjustment(uint32_t requested, uint32_t applied, bool json_mode) {
    if (requested == applied) return;
    if (json_mode)
        std::cerr << "{\"type\":\"warning\",\"code\":\"sample_rate_adjusted\",\"requested_hz\":" << requested
                  << ",\"applied_hz\":" << applied << "}\n";
    else
        presentation::status(presentation::level::warn, "No exact clock plan for " + display_number(requested / 1e6, 6) +
            " MSPS; using nearest supported " + display_number(applied / 1e6, 6) + " MSPS.", std::cout);
}
// Progress is a terminal convenience, never part of redirected or JSON output.
class terminal_progress {
    bool enabled_ = false;
    size_t width_ = 0, columns_ = 79;
public:
    explicit terminal_progress(bool json_mode) {
#if defined(_WIN32) && !defined(OCH_CLI_TEST)
        DWORD stdout_mode = 0, stderr_mode = 0;
        enabled_ = !json_mode && GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &stdout_mode)
            && GetConsoleMode(GetStdHandle(STD_ERROR_HANDLE), &stderr_mode);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (enabled_ && GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &info))
            columns_ = static_cast<size_t>(std::max(20, int(info.srWindow.Right - info.srWindow.Left)));
#elif defined(__linux__) && !defined(OCH_CLI_TEST)
        const char* term=std::getenv("TERM");
        enabled_=!json_mode && isatty(STDOUT_FILENO) && isatty(STDERR_FILENO) && term && std::strcmp(term,"dumb")!=0;
        winsize size{};
        if(enabled_ && ioctl(STDERR_FILENO,TIOCGWINSZ,&size)==0 && size.ws_col)
            columns_=std::max<size_t>(20,size.ws_col-1);
#else
        (void)json_mode;
#endif
    }
    bool enabled() const { return enabled_; }
    void clear() {
        if (width_) { std::cerr << '\r' << std::string(width_, ' ') << '\r' << std::flush; width_ = 0; }
    }
    void update(const std::pair<std::string, size_t>& line) {
        if (!enabled_) return;
        // Styled text cannot be cut safely; fall back to plain text if narrow.
        std::string text = line.first; size_t visible = line.second;
        if (visible > columns_) { text = text.substr(0, columns_); visible = columns_; }
        std::cerr << '\r' << text;
        if (width_ > visible) std::cerr << std::string(width_ - visible, ' ');
        std::cerr << std::flush;
        width_ = visible;
    }
    ~terminal_progress() { clear(); }
};
// Colour and Unicode require an interactive VT terminal. Redirected output
// stays plain ASCII on both platforms.
presentation::style terminal_style(bool stdout_handle, bool no_color) {
    presentation::style st;
#if defined(_WIN32) && !defined(OCH_CLI_TEST)
    HANDLE handle = GetStdHandle(stdout_handle ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode)) return st;
    const bool vt = (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) ||
        SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    st.unicode = vt;
    st.color = vt && !no_color;
#elif defined(__linux__) && !defined(OCH_CLI_TEST)
    const char* term=std::getenv("TERM");
    const bool vt=isatty(stdout_handle?STDOUT_FILENO:STDERR_FILENO) && term && std::strcmp(term,"dumb")!=0;
    const char* locale=std::setlocale(LC_CTYPE,nullptr);
    st.unicode=vt && locale && (std::strstr(locale,"UTF-8") || std::strstr(locale,"utf8"));
    st.color=vt && !no_color && !std::getenv("NO_COLOR");
#else
    (void)stdout_handle; (void)no_color;
#endif
    return st;
}

unsigned channel_count(uint32_t mask) { return mask == 3 ? 2u : 1u; }
och_format parse_format(const std::string& text) {
    if (text == "packed12") return OCH_PACKED12;
    if (text == "sc16") return OCH_SC16;
    if (text == "fc32") return OCH_FC32;
    if (text == "sc8") return OCH_SC8;
    throw std::invalid_argument("format must be packed12, sc16, fc32 or sc8");
}
std::string format_name(och_format value) {
    return value == OCH_PACKED12 ? "packed12" : value == OCH_SC16 ? "sc16" : value == OCH_SC8 ? "sc8" : "fc32";
}
unsigned wire_sample_bytes(const och_config& c) { return c.usb_iq_bits==8?2u:3u; }
uint32_t u32(const std::string& value) { return static_cast<uint32_t>(integer(value, UINT32_MAX)); }
void apply_rf(const options& opt, och_config& c) {
    if(opt.has("iq-bits")) {
        c.usb_iq_bits=u32(opt.get("iq-bits"));
        if(c.usb_iq_bits!=8 && c.usb_iq_bits!=12)throw std::invalid_argument("iq-bits must be 8 or 12");
    }
    if (opt.has("sample-rate")) c.sample_rate = u32(opt.get("sample-rate"));
    if (!c.sample_rate) throw std::invalid_argument("sample rate must be positive");
    if (opt.has("lo-frequency")) c.rx_frequency = c.tx_frequency = integer(opt.get("lo-frequency"), 6000000000ull);
    if (opt.has("rx-lo-frequency")) c.rx_frequency = integer(opt.get("rx-lo-frequency"), 6000000000ull);
    if (opt.has("tx-lo-frequency")) c.tx_frequency = integer(opt.get("tx-lo-frequency"), 6000000000ull);
    if (opt.has("bandwidth")) c.rx_bandwidth = c.tx_bandwidth = u32(opt.get("bandwidth"));
    if (opt.has("rx-bw")) c.rx_bandwidth = u32(opt.get("rx-bw"));
    if (opt.has("tx-bw")) c.tx_bandwidth = u32(opt.get("tx-bw"));
    if (opt.has("gain")) { c.rx_gain[0] = c.rx_gain[1] = number(opt.get("gain")); c.rx_agc = 0; }
    if (opt.has("agc")) {
        std::string mode = opt.get("agc");
        if (mode == "manual") c.rx_agc = 0;
        else if (mode == "fast") c.rx_agc = 1;
        else if (mode == "slow") c.rx_agc = 2;
        else if (mode == "hybrid") c.rx_agc = 3;
        else throw std::invalid_argument("agc must be manual, fast, slow or hybrid");
    }
    if (opt.has("attenuation")) c.tx_attenuation[0] = c.tx_attenuation[1] = number(opt.get("attenuation"));
    if (opt.has("channels")) c.rx_mask = c.tx_mask = channels(opt.get("channels"));
    if (opt.has("rx-channels")) c.rx_mask = channels(opt.get("rx-channels"));
    if (opt.has("tx-channels")) c.tx_mask = channels(opt.get("tx-channels"));
    if(c.usb_iq_bits==8 && (c.rx_mask!=3 || c.tx_mask!=3))
        throw std::invalid_argument("8-bit USB IQ requires both RX/TX channel masks=3");
}
const std::set<std::string> rf_options = {"sample-rate", "channels", "rx-channels", "tx-channels", "lo-frequency",
    "rx-lo-frequency", "tx-lo-frequency", "bandwidth", "rx-bw", "tx-bw", "gain", "agc", "attenuation", "iq-bits"};
std::string config_json(const och_config& c) {
    std::ostringstream s;
    s << "{\"sample_rate\":" << c.sample_rate << ",\"rx_mask\":" << c.rx_mask << ",\"tx_mask\":" << c.tx_mask
      << ",\"rx_frequency\":" << c.rx_frequency << ",\"tx_frequency\":" << c.tx_frequency
      << ",\"rx_bandwidth\":" << c.rx_bandwidth << ",\"tx_bandwidth\":" << c.tx_bandwidth
      << ",\"rx_gain\":[" << c.rx_gain[0] << ',' << c.rx_gain[1] << "],\"tx_attenuation\":["
      << c.tx_attenuation[0] << ',' << c.tx_attenuation[1] << "],\"rx_agc\":" << c.rx_agc
      << ",\"usb_iq_bits\":" << (c.usb_iq_bits?c.usb_iq_bits:12) << '}'; return s.str();
}
std::string power_json(const och_power_status& p) {
    const bool valid = p.snapshot_valid != 0 && p.conflicting_snapshots == 0;
    const bool sink = valid && p.attached_state == 2;
    const char* supports = sink && p.supports_3a == 1 ? "true" : sink && p.supports_3a == 0 ? "false" : "null";
    std::ostringstream out;
    out << "{\"source\":\"startup_log\",\"live\":false,\"log_endpoint_present\":"
        << (p.log_endpoint_present ? "true" : "false")
        << ",\"snapshot_valid\":" << (valid ? "true" : "false")
        << ",\"conflicting_snapshots\":" << (p.conflicting_snapshots ? "true" : "false")
        << ",\"attached_state\":" << (valid ? std::to_string(p.attached_state) : "null")
        << ",\"current_detected\":" << (valid ? std::to_string(p.current_detected) : "null")
        << ",\"source_current_ma\":" << (sink && p.source_current_ma >= 0 ? std::to_string(p.source_current_ma) : "null")
        << ",\"supports_3a\":" << supports
        << ",\"raw_08\":" << (valid ? std::to_string(p.raw_08) : "null")
        << ",\"raw_09\":" << (valid ? std::to_string(p.raw_09) : "null")
        << ",\"raw_0a\":" << (valid ? std::to_string(p.raw_0a) : "null") << '}';
    return out.str();
}
void print_power(const och_power_status& p) {
    presentation::power(p, std::cout);
}
std::string firmware_json(const och_firmware_info& f);
void print_firmware(const och_firmware_info& f);
std::string firmware_version(uint32_t v);
std::string time_json(const och_time_status& t) {
    std::ostringstream out;
    out << "{\"supported\":" << (t.capable ? "true" : "false");
    if (t.capable) {
        const char* src = t.source == OCH_TIME_SOURCE_EXTERNAL ? "external" : t.source == OCH_TIME_SOURCE_INTERNAL ? "internal" : "none";
        out << ",\"valid\":" << (t.valid ? "true" : "false") << ",\"armed\":" << (t.armed ? "true" : "false")
            << ",\"source\":" << json(src) << ",\"now\":" << t.now << ",\"last_pps\":" << t.last_pps
            << ",\"pps_count\":" << t.pps_count << ",\"tick_rate\":" << t.tick_rate
            << ",\"ref40_present\":" << (t.ref_present ? "true" : "false")
            << ",\"fpga_version\":\"0x" << std::hex << std::setw(8) << std::setfill('0') << t.fpga_version << std::dec << '"';
    }
    out << '}';
    return out.str();
}
// och info: one screen, grouped; details stay in och power / och firmware.
void print_device_summary(const och_device_info& i, const och_firmware_info* f, const och_time_status* t,
                          const och_power_status* p) {
    using presentation::tone;
    namespace d = presentation::detail;
    auto& o = std::cout;
    presentation::banner(o);
    presentation::section("Device", o);

    presentation::group("Identity", o);
    presentation::field("Name", d::nonempty(d::text(i.name)), o, tone::accent);
    presentation::field("Serial", d::nonempty(d::text(i.serial)), o);
    presentation::field("Driver", d::nonempty(d::text(i.driver)), o);
    const auto mbps = i.usb_speed_mbps;
    presentation::field("USB link", !mbps ? std::string("Unknown") :
        (mbps >= 1000 ? d::fixed(mbps / 1000.0, 1) + " Gbit/s" : std::to_string(mbps) + " Mbit/s") + "  " +
        (mbps >= 10000 ? "SuperSpeed+" : mbps >= 5000 ? "SuperSpeed" : mbps >= 480 ? "High-Speed (USB 2.0)" : "Full-Speed, unsupported"),
        o, mbps >= 5000 ? tone::good : tone::warn);

    presentation::group("Firmware", o);
    presentation::field("Zynq SDK", d::nonempty(d::text(i.firmware)), o);
    if (t && t->capable && t->fpga_version)
        presentation::field("FPGA IP", "CH32H417_IP " + std::to_string(t->fpga_version >> 24) + "." +
                            std::to_string((t->fpga_version >> 16) & 255) + "  (framed 16 KiB, hardware time)", o);
    if (f) {
        if (f->mode == OCH_FW_MODE_APP)
            presentation::field("CH32 bridge", firmware_version(f->running_version) + "  (bootloader " +
                                firmware_version(f->bootloader_version) + ")", o);
        else presentation::field("CH32 bridge", f->mode == OCH_FW_MODE_BOOTLOADER ? "Bootloader mode" :
                                 "Legacy, no USB update", o, tone::warn);
    }

    presentation::group("RF", o);
    presentation::field("Channels", std::to_string(i.rx_channels) + " RX / " + std::to_string(i.tx_channels) + " TX", o);
    presentation::field("Max sample rate", d::rate(i.max_sample_rate), o);

    if (t && t->capable) {
        presentation::group("Time base", o);
        const char* src = t->source == OCH_TIME_SOURCE_EXTERNAL ? "External, J6" :
            t->source == OCH_TIME_SOURCE_INTERNAL ? "Internal, AD9361 40 MHz" : "None";
        presentation::field("State", t->valid ? "Valid" : t->armed ? "Armed, loads on next PPS" : "Not set",
                            o, t->valid ? tone::good : tone::muted);
        presentation::field("PPS source", src, o);
        if (t->source) presentation::field("PPS count", d::grouped(t->pps_count), o);
        if (t->valid && t->tick_rate)
            presentation::field("Now", d::grouped(t->now) + "  (" + d::fixed(double(t->now) / t->tick_rate, 1) + " s at " +
                                d::fixed(t->tick_rate / 1e6, 3) + " MSPS)", o);
    }

    if (p) {
        presentation::group("Power", o);
        const bool valid = p->snapshot_valid == 1 && p->conflicting_snapshots == 0;
        const bool sink = valid && p->attached_state == 2;
        presentation::field("USB-C at startup", !valid ? std::string("Unknown (see och power)") :
            !sink ? std::string("Not attached as sink") :
            (p->source_current_ma >= 0 ? std::to_string(p->source_current_ma) + " mA available" : std::string("USB default current")),
            o, !valid ? tone::muted : p->supports_3a == 1 ? tone::good : tone::warn);
    }
    presentation::end_group(o);
    o << '\n';
    if (f && f->mode == OCH_FW_MODE_BOOTLOADER)
        presentation::status(presentation::level::warn, "Bridge is in its bootloader: och firmware boot, or och firmware update FILE.", o);
    else if (f && f->mode == OCH_FW_MODE_LEGACY)
        presentation::status(presentation::level::warn, "Bridge firmware predates 1.0.0: flash Merge.bin once with WCH-Link.", o);
    presentation::note("Link speed is negotiated; RX and TX share one USB link and sample clock.", o);
}
void print_info(const och_device_info& i, bool json_mode, const och_power_status* power = nullptr,
                const och_firmware_info* bridge = nullptr, const och_time_status* time = nullptr) {
    if (json_mode) {
        std::cout << "{\"serial\":" << json(i.serial) << ",\"name\":" << json(i.name)
            << ",\"driver\":" << json(i.driver) << ",\"firmware\":" << json(i.firmware)
            << ",\"usb_speed_mbps\":" << i.usb_speed_mbps << ",\"rx_channels\":" << i.rx_channels
            << ",\"tx_channels\":" << i.tx_channels << ",\"max_sample_rate\":" << i.max_sample_rate
            << ",\"sample_rate_scope\":\"rf_capability\""
            << ",\"hardware_timestamps\":" << (i.hardware_timestamps ? "true" : "false");
        if (usb2_reference::applies(i)) {
            std::cout << ",\"usb2_reference\":{\"scope\":\"single_host_observation\",\"date\":"
                      << json(usb2_reference::date) << ",\"host\":" << json(usb2_reference::host) << ",\"profiles\":[";
            bool first = true;
            for (const auto& e : usb2_reference::entries) if (e.rate_hz) {
                if (!first) std::cout << ',';
                first = false;
                std::cout << "{\"mode\":" << json(e.id) << ",\"iq_bits\":" << e.bits
                    << ",\"rx_mask\":" << (e.rx_channels == 2 ? 3 : e.rx_channels)
                    << ",\"tx_mask\":" << (e.tx_channels == 2 ? 3 : e.tx_channels)
                    << ",\"sample_rate_hz\":" << e.rate_hz << ",\"test_seconds\":" << e.seconds << '}';
            }
            std::cout << "]}";
        }
        if (bridge) std::cout << ",\"bridge\":" << firmware_json(*bridge);
        if (time) std::cout << ",\"time\":" << time_json(*time);
        if (power) std::cout << ",\"power\":" << power_json(*power);
        std::cout << '}';
    } else if (power || bridge || time) {
        print_device_summary(i, bridge, time, power);
    } else {
        presentation::device(i, false, std::cout);
    }
}
std::string stats_json(const och_stats& s, unsigned channels_count) {
    double seconds = static_cast<double>(s.elapsed_ns) / 1e9;
    std::ostringstream out; out << std::setprecision(10);
    out << "{\"usb_bytes\":" << s.usb_bytes << ",\"samples_per_channel\":" << s.samples
        << ",\"elapsed_seconds\":" << seconds
        << ",\"usb_MBps\":" << (seconds > 0 ? s.usb_bytes / seconds / 1e6 : 0)
        << ",\"channels\":" << channels_count << ",\"host_dropped_samples\":" << s.host_dropped_samples
        << ",\"headers\":" << s.headers << ",\"resyncs\":" << s.resyncs
        << ",\"host_queue_peak_bytes\":" << s.host_queue_peak_bytes
        << ",\"transfer_errors\":" << s.transfer_errors << ",\"device_overflows\":" << s.device_overflows
        << ",\"device_underflows\":" << s.device_underflows << ",\"event_drops\":" << s.event_drops
        << ",\"tx_starvation_events\":" << s.tx_starvation_events
        << ",\"tx_padding_samples\":" << s.tx_padding_samples << '}'; return out.str();
}

class tx_source {
    och_format format_;
    unsigned channels_;
    size_t bytes_per_sample_, offset_ = 0, samples_ = 0;
    bool loop_ = false, tone_ = false;
    std::ifstream file_;
    std::vector<uint8_t> buffer_;
public:
    double actual_tone = 0;
    tx_source(const options& opt, const och_config& c, och_format format, uint32_t block_bytes)
        : format_(format), channels_(channel_count(c.tx_mask)), bytes_per_sample_(och_sample_size(format) * channels_) {
        loop_ = opt.has("loop"); tone_ = !opt.has("file");
        size_t block_samples = block_bytes / (wire_sample_bytes(c) * channels_);
        if (!tone_) {
            auto path = std::filesystem::u8path(opt.get("file"));
            if (!std::filesystem::is_regular_file(path)) throw std::invalid_argument("TX input is not a regular file");
            uint64_t bytes = std::filesystem::file_size(path);
            if (!bytes || bytes % bytes_per_sample_) throw std::invalid_argument("TX file is empty or ends inside an interleaved IQ frame");
            if (!loop_ && bytes / bytes_per_sample_ < 4 * block_samples)
                throw std::invalid_argument("TX file is too short for initial prefill; use --loop or supply at least four USB blocks");
            file_.open(path, std::ios::binary);
            if (!file_) throw std::runtime_error("cannot open TX file");
            buffer_.resize(block_samples * bytes_per_sample_);
            refill(); return;
        }
        uint64_t frequency = integer(opt.get("tone", "1M"), c.sample_rate / 2);
        double amplitude = number(opt.get("amplitude", "0.25"));
        if (amplitude < 0 || amplitude > 1) throw std::invalid_argument("amplitude must be between 0 and 1");
        uint64_t period = frequency ? c.sample_rate / std::gcd<uint64_t>(frequency, c.sample_rate) : 1;
        uint64_t cycles = frequency ? frequency / std::gcd<uint64_t>(frequency, c.sample_rate) : 0;
        if (period > 65536) {
            period = 65536; cycles = static_cast<uint64_t>(std::llround(static_cast<double>(frequency) * period / c.sample_rate));
        }
        actual_tone = static_cast<double>(cycles) * c.sample_rate / period;
        samples_ = ((block_samples + period - 1) / period) * period;
        buffer_.resize(samples_ * bytes_per_sample_);
        constexpr double tau = 6.283185307179586476925286766559;
        for (size_t n = 0; n < samples_; ++n) {
            double phase = tau * cycles * (n % period) / period;
            int16_t iv = static_cast<int16_t>(std::lround(std::cos(phase) * amplitude * 2047));
            int16_t qv = static_cast<int16_t>(std::lround(std::sin(phase) * amplitude * 2047));
            for (unsigned ch = 0; ch < channels_; ++ch) {
                uint8_t* dst = buffer_.data() + n * bytes_per_sample_ + ch * och_sample_size(format_);
                if (format_ == OCH_PACKED12) {
                    uint16_t i = static_cast<uint16_t>(iv) & 4095, q = static_cast<uint16_t>(qv) & 4095;
                    dst[0] = static_cast<uint8_t>(i >> 4); dst[1] = static_cast<uint8_t>((i << 4) | (q >> 8)); dst[2] = static_cast<uint8_t>(q);
                } else if (format_ == OCH_SC8) {
                    dst[0]=static_cast<uint8_t>((static_cast<uint16_t>(iv)&4095u)>>4);
                    dst[1]=static_cast<uint8_t>((static_cast<uint16_t>(qv)&4095u)>>4);
                } else if (format_ == OCH_SC16) {
                    int16_t iq[2] = {static_cast<int16_t>(iv * 16), static_cast<int16_t>(qv * 16)};
                    std::memcpy(dst, iq, sizeof(iq));
                } else {
                    float iq[2] = {iv / 2048.0f, qv / 2048.0f}; std::memcpy(dst, iq, sizeof(iq));
                }
            }
        }
    }
    bool refill() {
        offset_ = 0;
        if (tone_) return true;
        size_t total = 0;
        while (total < buffer_.size()) {
            file_.read(reinterpret_cast<char*>(buffer_.data() + total), static_cast<std::streamsize>(buffer_.size() - total));
            size_t got = static_cast<size_t>(file_.gcount()); total += got;
            if (file_.bad()) throw std::runtime_error("TX file read failed");
            if (total == buffer_.size()) break;
            if (!loop_) break;
            file_.clear(); file_.seekg(0);
            if (!file_) throw std::runtime_error("TX file rewind failed");
        }
        samples_ = total / bytes_per_sample_; return samples_ != 0;
    }
    bool available() { return offset_ < samples_ || refill(); }
    const void* data() const { return buffer_.data() + offset_ * bytes_per_sample_; }
    size_t remaining() const { return samples_ - offset_; }
    void consume(size_t count) { offset_ += count; }
};

struct workers {
    std::atomic<bool> stop{false}, tx_stop{false}, rx_stop{false}, failed{false}, eof{false};
    std::thread rx, tx;
    std::mutex error_mutex;
    std::exception_ptr error;
    void capture() {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!error) error = std::current_exception();
        failed = true; stop = true;
    }
    void join() {
        stop = true;
        if (tx.joinable()) tx.join();
        if (rx.joinable()) rx.join();
    }
    ~workers() { join(); }
};

int run_stream(const options& opt) {
    auto names = rf_options;
    for (auto n : {"seconds", "file", "format", "tone", "amplitude", "buffer-mib", "queue", "block", "loop", "overwrite", "rx", "tx", "fail-fast"}) names.insert(n);
    allowed(opt, names);
    bool bench = opt.command == "bench", do_rx = opt.command == "rx" || (bench && opt.has("rx"));
    bool do_tx = opt.command == "tx" || (bench && opt.has("tx"));
    if (bench && !do_rx && !do_tx) throw std::invalid_argument("bench needs --rx, --tx or both");
    if (!bench && (opt.has("rx") || opt.has("tx"))) throw std::invalid_argument("--rx and --tx are bench options");
    if (!bench && opt.has("fail-fast")) throw std::invalid_argument("--fail-fast is a bench option");
    if (bench && opt.has("file")) throw std::invalid_argument("bench does not write/read files; use rx or tx");
    if (!do_tx && (opt.has("tone") || opt.has("amplitude") || opt.has("loop"))) throw std::invalid_argument("tone/amplitude/loop require TX");
    if (opt.has("file") && opt.has("tone")) throw std::invalid_argument("choose a TX file or a tone");
    if (opt.has("file") && opt.has("amplitude")) throw std::invalid_argument("amplitude applies only to a generated tone");
    if (opt.has("loop") && !opt.has("file")) throw std::invalid_argument("--loop requires a TX file");
    if (!do_rx && opt.has("overwrite")) throw std::invalid_argument("--overwrite is only for RX captures");
    if (opt.command == "rx" && !opt.has("file")) throw std::invalid_argument("rx requires --file PATH; use bench --rx to discard samples");
    if (opt.command == "rx" && opt.has("tx-channels")) throw std::invalid_argument("--tx-channels requires bench --tx or config");
    if (opt.command == "tx" && opt.has("rx-channels")) throw std::invalid_argument("--rx-channels requires bench --rx or config");
    double seconds = number(opt.get("seconds", do_tx && opt.has("file") ? "0" : "10"));
    if (seconds < 0 || seconds > 86400 * 365.0) throw std::invalid_argument("seconds must be 0..31536000");
    const auto requested_bits=u32(opt.get("iq-bits","12"));
    if(requested_bits!=8 && requested_bits!=12)throw std::invalid_argument("iq-bits must be 8 or 12");
    och_format format = parse_format(opt.get("format", bench ? (requested_bits==8?"sc8":"packed12") : "sc16"));
    if (bench && format != (requested_bits==8?OCH_SC8:OCH_PACKED12))
        throw std::invalid_argument("bench requires the native wire format: packed12 for 12 bits, sc8 for 8 bits");
    auto args = och::stream_args(OCH_RX);
    if (opt.has("buffer-mib")) args.host_buffer_bytes = static_cast<uint32_t>(integer(opt.get("buffer-mib"), 512) * 1048576);
    if (opt.has("queue")) args.usb_queue_depth = u32(opt.get("queue"));
    if (opt.has("block")) args.transfer_bytes = u32(opt.get("block"));
    if (!args.transfer_bytes || args.transfer_bytes > 1048576 || args.transfer_bytes % 16384)
        throw std::invalid_argument("block must be a multiple of 16384, at most 1048576");
    if (args.usb_queue_depth < 2 || args.usb_queue_depth > 128) throw std::invalid_argument("queue must be 2..128");
    if (args.host_buffer_bytes < args.transfer_bytes * 4ull) throw std::invalid_argument("host buffer must hold at least four USB blocks");

    och_config config = och::default_config();
    config.sample_rate = 30720000;
    config.rx_mask = do_rx ? 3 : 0; config.tx_mask = do_tx ? 3 : 0;
    apply_rf(opt, config);
    if (!do_rx) config.rx_mask = 0;
    if (!do_tx) config.tx_mask = 0;
    if(config.usb_iq_bits==8 && (config.rx_mask!=3 || config.tx_mask!=3))
        throw std::invalid_argument("8-bit USB IQ requires bench --rx --tx and both channel masks=3");
    if(format==OCH_SC8 && config.usb_iq_bits!=8)throw std::invalid_argument("sc8 requires --iq-bits 8");
    std::unique_ptr<tx_source> source;
    if (do_tx) source = std::make_unique<tx_source>(opt, config, format, args.transfer_bytes);
    std::ofstream output, metadata;
    if (do_rx && opt.has("file")) {
        auto path = std::filesystem::u8path(opt.get("file"));
        auto sidecar = std::filesystem::u8path(opt.get("file") + ".metadata.jsonl");
        if (!opt.has("overwrite") && (std::filesystem::exists(path) || std::filesystem::exists(sidecar)))
            throw std::invalid_argument("capture or metadata file already exists; use --overwrite to replace it");
    }
    och::device device(opt.get("serial"));
    device.configure(config);
    const uint32_t requested_rate = config.sample_rate;
    config.sample_rate = device.config().sample_rate;
    // The test tone is synthesized for the rate actually applied.
    if (do_tx && source && !opt.has("file") && config.sample_rate != requested_rate)
        source = std::make_unique<tx_source>(opt, config, format, args.transfer_bytes);
    if (!opt.has("json")) {
        const std::string mode = do_rx && do_tx ? "RX + TX" : do_rx ? "RX" : "TX";
        presentation::title(opt.command, bench ? "throughput benchmark, " + mode :
                            do_rx ? "IQ capture to file" : opt.has("file") ? "IQ file playback" : "generated tone", std::cout);
        presentation::section(bench ? "Stream benchmark" : do_rx ? "RX capture" : "TX transmit", std::cout);
        presentation::group("Session", std::cout);
        presentation::field("Device", device.info().serial, std::cout, presentation::tone::accent);
        presentation::field("Direction", mode, std::cout);
        presentation::field("Duration", seconds > 0 ? display_number(seconds) + " s" :
                            do_tx && opt.has("file") && !opt.has("loop") ? "Until end of file" : "Until Ctrl+C", std::cout);
        presentation::field("Sample format", format_name(format) + "  (USB wire " + (config.usb_iq_bits==8?"sc8)":"packed12)"), std::cout);
        if (opt.has("file")) presentation::field(do_rx ? "Capture file" : "Input file", opt.get("file"), std::cout);
        presentation::group("Transport", std::cout);
        const auto link = device.info().usb_speed_mbps;
        presentation::field("USB link", link >= 1000 ? display_number(link / 1000.0, 1) + " Gbit/s" : std::to_string(link) + " Mbit/s", std::cout);
        const double wire_MBps = static_cast<double>(config.sample_rate) * wire_sample_bytes(config) *
            ((do_rx ? channel_count(config.rx_mask) : 0) + (do_tx ? channel_count(config.tx_mask) : 0)) / 1e6;
        presentation::field("RX + TX wire demand", display_number(wire_MBps, 2) + " MB/s", std::cout,
                            wire_MBps > 533 ? presentation::tone::warn : presentation::tone::plain);
        if (wire_MBps > 533)
            presentation::status(presentation::level::warn, "Above the ~533 MB/s FPGA bridge bus; expect RX gaps and TX underruns.", std::cout);
        presentation::usb2_stream_reference(device.info(), config, std::cout);
        presentation::field("Host buffer", display_number(args.host_buffer_bytes / 1048576.0, 0) + " MiB per direction", std::cout);
        presentation::field("USB requests", std::to_string(args.usb_queue_depth) + " x " + std::to_string(args.transfer_bytes) + " bytes", std::cout);
        presentation::end_group(std::cout);
        presentation::config(config, std::cout);
        report_rate_adjustment(requested_rate, config.sample_rate, false);
        std::cout << '\n';
        presentation::status(presentation::level::prep, "Allocating buffers and preparing streams.", std::cout);
        std::cout << std::flush;
    }
    if (opt.has("json")) {
        report_rate_adjustment(requested_rate, config.sample_rate, true);
        const auto* ref = usb2_reference::find(device.info(), config);
        if (ref && config.sample_rate > ref->rate_hz)
            std::cerr << "{\"type\":\"warning\",\"code\":\"usb2_reference_exceeded\",\"reference_rate_hz\":"
                      << ref->rate_hz << ",\"test_seconds\":" << ref->seconds
                      << ",\"scope\":\"single_host_observation\"}\n";
    }
    if (do_rx && opt.has("file")) {
        output.open(std::filesystem::u8path(opt.get("file")), std::ios::binary | std::ios::trunc);
        metadata.open(std::filesystem::u8path(opt.get("file") + ".metadata.jsonl"), std::ios::trunc);
        if (!output || !metadata) throw std::runtime_error("cannot create RX capture and metadata files");
        metadata << "{\"type\":\"capture\",\"version\":" << json(och_version()) << ",\"format\":" << json(format_name(format))
            << ",\"config\":" << config_json(config) << ",\"hardware_timestamps\":"
            << (device.info().hardware_timestamps ? "true" : "false") << "}\n";
    }
    std::unique_ptr<och::stream> rx, tx;
    if (do_rx) { args.direction = OCH_RX; args.format = format; rx = std::make_unique<och::stream>(device, args); }
    if (do_tx) { args.direction = OCH_TX; args.format = format; tx = std::make_unique<och::stream>(device, args); }
    workers work;
    if (source) {
        if (opt.has("json")) std::cerr << "{\"type\":\"tx_source\",\"source\":" << json(opt.has("file") ? opt.get("file") : "tone")
            << ",\"actual_tone_Hz\":" << source->actual_tone << "}\n";
        else {
            presentation::group("TX source", std::cout);
            presentation::field("Source", opt.has("file") ? opt.get("file") : "Generated complex sine", std::cout);
            if (!opt.has("file")) {
                presentation::field("Actual tone", display_number(source->actual_tone, 3) + " Hz", std::cout);
                presentation::field("Amplitude", opt.get("amplitude", "0.25") + " of full scale", std::cout);
            } else presentation::field("Playback", opt.has("loop") ? "Loop until stopped" : "Once, drain at EOF", std::cout);
            presentation::end_group(std::cout);
            std::cout << std::flush;
        }
    }
    if (tx) {
        size_t prefill = 4ull * args.transfer_bytes / (wire_sample_bytes(config) * channel_count(config.tx_mask)), total = 0;
        auto deadline = clock_type::now() + std::chrono::seconds(10);
        while (total < prefill && !interrupted) {
            if (!source->available()) throw std::runtime_error("TX source ended during prefill");
            size_t sent = 0;
            int rc = tx->send(source->data(), std::min(source->remaining(), prefill - total), sent, 500);
            source->consume(sent); total += sent;
            if (rc != OCH_OK && rc != OCH_TIMEOUT) och::check(rc);
            if (clock_type::now() > deadline) throw std::runtime_error("TX prefill timed out");
        }
        if (interrupted) return 130;
    }
    auto rx_worker = [&] {
        try {
            const size_t frame_bytes = och_sample_size(format) * channel_count(config.rx_mask);
            std::vector<uint8_t> data(65536 * frame_bytes);
            uint64_t file_samples = 0, previous_epoch = UINT64_MAX;
            while (!work.stop && !work.rx_stop) {
                och_metadata m{};
                size_t count = 0, bytes = 0;
                const void* payload = nullptr;
                och::rx_lease lease;
                int rc;
                if (bench || (format == OCH_PACKED12 && config.usb_iq_bits!=8) || format==OCH_SC8) {
                    rc = lease.acquire(*rx, 200);
                    if (rc == OCH_OK) { payload = lease.data(); bytes = lease.bytes(); count = bytes / (wire_sample_bytes(config) * channel_count(config.rx_mask)); m = lease.metadata(); }
                } else {
                    rc = rx->recv(data.data(), data.size() / frame_bytes, count, m, 200);
                    payload = data.data(); bytes = count * frame_bytes;
                }
                if (rc == OCH_TIMEOUT) continue;
                if (rc == OCH_STOPPED && (work.stop || work.rx_stop)) break;
                och::check(rc);
                if (output.is_open()) {
                    output.write(static_cast<const char*>(payload), static_cast<std::streamsize>(bytes));
                    if (!output) throw std::runtime_error("RX output write failed (disk full or too slow)");
                    if (m.epoch != previous_epoch || (m.flags & (OCH_META_START | OCH_META_DISCONTINUITY | OCH_META_HOST_OVERFLOW | OCH_META_DEVICE_OVERFLOW))) {
                        metadata << "{\"type\":\"boundary\",\"file_sample\":" << file_samples
                            << ",\"file_byte\":" << file_samples * frame_bytes << ",\"epoch\":" << m.epoch
                            << ",\"sample_index\":" << m.sample_index << ",\"flags\":" << m.flags
                            << ",\"host_dropped_samples\":" << m.host_dropped_samples
                            << ",\"device_dropped_samples\":" << m.device_dropped_samples
                            << ",\"time_ticks\":" << ((m.flags & OCH_META_HAS_TIME) ? std::to_string(m.time_ticks) : std::string("null"))
                            << ",\"tick_rate\":" << m.tick_rate
                            << ",\"host_monotonic_ns\":" << m.host_monotonic_ns << "}\n";
                        if (!metadata) throw std::runtime_error("RX metadata write failed");
                    }
                }
                file_samples += count; previous_epoch = m.epoch;
            }
        } catch (...) { work.capture(); }
    };
    if (tx) {
        work.tx = std::thread([&] {
            try {
                while (!work.stop && !work.tx_stop) {
                    if (!source->available()) { work.eof = true; break; }
                    size_t sent = 0;
                    int rc = tx->send(source->data(), source->remaining(), sent, 200);
                    source->consume(sent);
                    if (rc == OCH_TIMEOUT) continue;
                    if (rc == OCH_STOPPED && work.stop) break;
                    och::check(rc);
                }
            } catch (...) { work.capture(); }
        });
        // The producer must already be active when the hardware starts. Four
        // wire blocks satisfy the minimum protocol prefill but cover only a few
        // milliseconds at full rate; build a larger scheduler cushion first.
        const uint64_t warm_bytes = std::min<uint64_t>(32ull * 1048576, args.host_buffer_bytes / 2);
        auto warm_deadline = clock_type::now() + std::chrono::seconds(10);
        while (!work.eof && !work.failed && !interrupted) {
            if (tx->stats().host_queue_bytes >= warm_bytes) break;
            if (clock_type::now() > warm_deadline) throw std::runtime_error("TX producer warm-up timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (work.failed) { work.join(); std::rethrow_exception(work.error); }
        if (interrupted) return 130;
    }
    if (rx) { rx->start(); work.rx = std::thread(rx_worker); }
    if (tx) tx->start();
    auto begin = clock_type::now();
    if (!opt.has("json")) {
        std::cerr << '\n';
        presentation::status(presentation::level::run, "Streaming started. Press Ctrl+C to stop cleanly.", std::cerr);
    }
    terminal_progress progress(opt.has("json"));
    auto previous_progress = begin;
    uint64_t previous_rx_bytes = progress.enabled() && rx ? rx->stats().usb_bytes : 0;
    uint64_t previous_tx_bytes = progress.enabled() && tx ? tx->stats().usb_bytes : 0;
    uint64_t event_count = 0, events_shown = 0, device_events = 0;  // device_events adds up SDK count=
    // Text mode prints the first few events of each type, then one roll-up
    // line per type and second, so a fault storm stays readable.
    constexpr uint64_t per_type_lines = 3;
    std::map<uint32_t, uint64_t> shown_by_type, held_by_type;
    std::map<uint32_t, och_event> last_held;
    auto last_rollup = begin;
    auto rollup = [&](bool force) {
        const auto now = clock_type::now();
        if (!force && now - last_rollup < std::chrono::seconds(1)) return;
        last_rollup = now;
        const double t = std::chrono::duration<double>(now - begin).count();
        for (auto& [type, held] : held_by_type) {
            if (!held) continue;
            progress.clear();
            presentation::event(last_held[type], t, std::cerr, held);
            held = 0;
        }
    };
    stream_assessment::startup_observer startup;
    stream_assessment::repeated_overflow_observer repeated_overflow;
    bool stopped_early = false;
    auto event_drain = [&](size_t budget) {  // bounded so a fault storm cannot stall the run loop
        och_event event{};
        for (size_t n = 0; n < budget && device.poll_event(event); ++n) {
            ++event_count;
            const uint64_t represented = event.count ? event.count : 1;
            device_events += represented;
            const double observed_seconds = std::chrono::duration<double>(clock_type::now() - begin).count();
            startup.observe(event, observed_seconds);
            if (opt.has("json")) std::cerr << "{\"type\":\"event\",\"sequence\":" << event.sequence
                << ",\"event_type\":" << event.type << ",\"epoch\":" << event.epoch
                << ",\"host_monotonic_ns\":" << event.host_monotonic_ns
                << ",\"observed_seconds\":" << std::setprecision(17) << observed_seconds
                << ",\"count\":" << represented
                << ",\"message\":" << json(event.message) << "}\n";
            else if (shown_by_type[event.type] < per_type_lines) {
                ++shown_by_type[event.type]; ++events_shown;
                progress.clear();
                presentation::event(event, observed_seconds, std::cerr);
            } else {
                held_by_type[event.type] += represented;
                last_held[event.type] = event;
            }
        }
        if (!opt.has("json")) rollup(false);
    };
    while (!interrupted && !work.failed) {
        event_drain(256);
        if (rx && opt.has("fail-fast")) {
            repeated_overflow.observe(rx->stats().device_overflows,
                                      std::chrono::duration<double>(clock_type::now() - begin).count());
            if (repeated_overflow.repeated) { stopped_early = true; break; }
        }
        auto now = clock_type::now();
        if (progress.enabled() && now - previous_progress >= std::chrono::seconds(1)) {
            double elapsed = std::chrono::duration<double>(now - begin).count();
            double interval = std::chrono::duration<double>(now - previous_progress).count();
            std::vector<std::pair<std::string, double>> rates;
            if (rx) {
                auto snapshot = rx->stats();
                rates.emplace_back("RX", (snapshot.usb_bytes - previous_rx_bytes) / interval / 1e6);
                previous_rx_bytes = snapshot.usb_bytes;
            }
            if (tx) {
                auto snapshot = tx->stats();
                rates.emplace_back("TX", (snapshot.usb_bytes - previous_tx_bytes) / interval / 1e6);
                previous_tx_bytes = snapshot.usb_bytes;
            }
            progress.update(presentation::progress(presentation::current(std::cerr), elapsed, seconds, rates));
            previous_progress = now;
        }
        if (seconds > 0 && std::chrono::duration<double>(clock_type::now() - begin).count() >= seconds) break;
        if (work.eof) {
            // EOF is set only after the final send, with no producer still writing.
            tx->flush(5000);
            // USB completion is not an RF timestamp. Give the small device FIFO
            // time to play before stop closes TX; padding remains explicit in stats.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    progress.clear();
    if (!opt.has("json")) presentation::status(presentation::level::stop, "Draining workers and closing streams.", std::cerr);
    work.tx_stop = true;
    if (work.tx.joinable()) work.tx.join();
    int stop_tx = tx ? och_stop(tx->native_handle(), 3000) : OCH_OK;
    work.rx_stop = true;
    int stop_rx = rx ? och_stop(rx->native_handle(), 3000) : OCH_OK;
    work.stop = true;
    work.join(); event_drain(SIZE_MAX);
    if (!opt.has("json")) rollup(true);
    if (work.error) std::rethrow_exception(work.error);
    och::check(stop_tx); och::check(stop_rx);
    och_stats rs{}, ts{};
    if (rx) rs = rx->stats();
    if (tx) ts = tx->stats();
    const auto startup_accepted = rx ? startup.accepted(rs) : 0;
    const auto rx_remaining = rx ? stream_assessment::remaining_faults(rs, true, startup_accepted) : 0;
    const auto tx_remaining = tx ? stream_assessment::remaining_faults(ts, false, startup_accepted) : 0;
    const bool stable = !interrupted && !stopped_early && !rx_remaining && !tx_remaining &&
        (!rx || stream_assessment::throughput_ok(rs, channel_count(config.rx_mask), config.sample_rate, wire_sample_bytes(config))) &&
        (!tx || stream_assessment::throughput_ok(ts, channel_count(config.tx_mask), config.sample_rate, wire_sample_bytes(config)));
    if (output.is_open()) { output.flush(); metadata << "{\"type\":\"end\",\"stats\":" << stats_json(rs, channel_count(config.rx_mask)) << "}\n"; metadata.flush();
        if (!output || !metadata) throw std::runtime_error("RX output flush failed"); }
    if (opt.has("json")) {
        std::cout << "{\"command\":" << json(opt.command) << ",\"config\":" << config_json(config)
            << ",\"format\":" << json(format_name(format)) << ",\"events\":" << event_count << ",\"device_events\":" << device_events
            << ",\"interrupted\":" << (interrupted ? "true" : "false")
            << ",\"stopped_early\":" << (stopped_early ? "true" : "false");
        std::cout << ",\"stability\":{\"startup_window_seconds\":" << stream_assessment::startup_seconds
            << ",\"policy\":\"recovered_rx_overflows_within_window\""
            << ",\"accepted_startup_overflows\":" << startup_accepted
            << ",\"rx_remaining_faults\":" << rx_remaining << ",\"tx_remaining_faults\":" << tx_remaining
            << ",\"pass\":" << (stable ? "true" : "false") << '}';
        if (rx) std::cout << ",\"rx\":" << stats_json(rs, channel_count(config.rx_mask));
        if (tx) std::cout << ",\"tx\":" << stats_json(ts, channel_count(config.tx_mask));
        std::cout << "}\n";
    } else {
        std::cout << std::flush;
        presentation::section("Stream complete", std::cout);
        presentation::field("Stop reason", interrupted ? "User interruption (Ctrl+C)" : stopped_early ? "Repeated RX overflows after startup" : work.eof ? "TX source reached end of file" : "Requested duration reached", std::cout);
        presentation::field("Events", presentation::detail::grouped(event_count) +
            (device_events > event_count ? " records, " + presentation::detail::grouped(device_events) + " device events" : std::string()) +
            (events_shown < event_count ? "  (" + presentation::detail::grouped(events_shown) + " listed, rest summarized)" : ""),
            std::cout, event_count ? presentation::tone::plain : presentation::tone::muted);
        if (startup_accepted) presentation::field("Startup RX gaps", std::to_string(startup_accepted) + "  (first second, tolerated)", std::cout);
        const unsigned wire = wire_sample_bytes(config);
        auto row = [&](const char* name, const och_stats& st, uint32_t mask) {
            const double secs = st.elapsed_ns / 1e9;
            const unsigned ch = channel_count(mask);
            return presentation::throughput_row{name, ch, config.sample_rate, secs, secs > 0 ? st.usb_bytes / secs / 1e6 : 0,
                static_cast<double>(config.sample_rate) * ch * wire / 1e6,
                st.transfer_errors + st.event_drops + (name[0] == 'R'
                    ? st.host_dropped_samples + st.device_overflows + st.resyncs
                    : st.device_underflows + st.tx_starvation_events + st.tx_padding_samples)};
        };
        std::vector<presentation::throughput_row> rows;
        if (rx) rows.push_back(row("RX", rs, config.rx_mask));
        if (tx) rows.push_back(row("TX", ts, config.tx_mask));
        std::cout << '\n';
        presentation::throughput(rows, std::cout);
        if (output.is_open()) {
            presentation::group("Saved files", std::cout);
            std::error_code size_error;
            const auto size = std::filesystem::file_size(std::filesystem::u8path(opt.get("file")), size_error);
            presentation::field("IQ data", opt.get("file"), std::cout, presentation::tone::accent);
            if (!size_error) presentation::field("IQ size", presentation::detail::bytes(size) + "  (" +
                presentation::detail::grouped(size) + " bytes, " + presentation::detail::grouped(
                size / (och_sample_size(format) * channel_count(config.rx_mask))) + " samples/ch)", std::cout);
            presentation::field("Boundary metadata", opt.get("file") + ".metadata.jsonl", std::cout);
            presentation::end_group(std::cout);
        }
        if (rx) presentation::stats(OCH_RX, rs, channel_count(config.rx_mask), config.sample_rate, std::cout, wire, startup_accepted);
        if (tx) presentation::stats(OCH_TX, ts, channel_count(config.tx_mask), config.sample_rate, std::cout, wire, 0);
    }
    return interrupted ? 130 : stopped_early ? 3 : 0;
}

std::string fir_json(const och_fir_config& f) {
    if (f.tap_count > 128) throw std::runtime_error("device returned an invalid FIR tap count");
    std::ostringstream out;
    out << "{\"direction\":" << json(f.direction == OCH_RX ? "rx" : "tx")
        << ",\"channel_mask\":" << f.channel_mask << ",\"enabled\":" << (f.enabled ? "true" : "false")
        << ",\"verified\":" << (f.verified ? "true" : "false") << ",\"factor\":" << f.factor
        << ",\"gain_db\":" << f.gain_db << ",\"tap_count\":" << f.tap_count << ",\"crc32\":" << f.crc32
        << ",\"coefficients\":[";
    for (uint32_t i = 0; i < f.tap_count; ++i) { if (i) out << ','; out << f.coefficients[i]; }
    out << "]}"; return out.str();
}
void print_fir(const och_fir_config& f) {
    if (f.tap_count > 128) throw std::runtime_error("device returned an invalid FIR tap count");
    presentation::fir(f, std::cout);
}
int run_fir(const options& opt) {
    allowed(opt, {"coefficients-only"}, true);
    const auto& p = opt.positional;
    if (p.empty() || (p.size() == 1 && p[0] == "status")) {
        if (opt.has("coefficients-only")) throw std::invalid_argument("--coefficients-only applies to fir load");
        och::device d(opt.get("serial"));
        auto rx = d.fir(OCH_RX), tx = d.fir(OCH_TX);
        if (opt.has("json")) std::cout << "{\"rx\":" << fir_json(rx) << ",\"tx\":" << fir_json(tx) << "}\n";
        else { print_fir(rx); print_fir(tx); }
        return 0;
    }
    if (p.size() < 2 || (p[0] != "rx" && p[0] != "tx" && p[0] != "both"))
        throw std::invalid_argument("use fir status, fir rx|tx|both on|off, or fir rx|tx load FILE");
    if (p[1] == "on" || p[1] == "off") {
        if (p.size() != 2 || opt.has("coefficients-only")) throw std::invalid_argument("fir on/off takes no file or coefficient options");
        och::device d(opt.get("serial"));
        auto rx = d.fir(OCH_RX), tx = d.fir(OCH_TX);
        bool enabled = p[1] == "on";
        d.set_fir_enabled(p[0] == "tx" ? rx.enabled != 0 : enabled,
                          p[0] == "rx" ? tx.enabled != 0 : enabled);
        rx = d.fir(OCH_RX); tx = d.fir(OCH_TX);
        if (opt.has("json")) std::cout << "{\"rx\":" << fir_json(rx) << ",\"tx\":" << fir_json(tx) << "}\n";
        else { print_fir(rx); print_fir(tx); }
        return 0;
    }
    if (p[1] != "load" || p.size() != 3 || p[0] == "both")
        throw std::invalid_argument("use fir rx|tx load FILE; each load changes one direction");
    auto direction = p[0] == "rx" ? OCH_RX : OCH_TX;
    // Validate the complete file before opening/touching the device.
    auto file = read_ftr(p[2], direction, opt.has("coefficients-only"));
    if (!file.ignored_profile_fields.empty()) {
        if (opt.has("json")) std::cerr << "{\"type\":\"notice\",\"message\":\"FTR clock/bandwidth metadata explicitly ignored; current RF profile was not imported\"}\n";
        else presentation::status(presentation::level::note,
            "FTR clock/bandwidth metadata ignored; only coefficients, gain and factor are loaded.", std::cerr);
    }
    och::device d(opt.get("serial")); d.set_fir(file.config);
    auto applied = d.fir(direction);
    if (opt.has("json")) {
        std::cout << "{\"operation\":\"load\",\"fir\":" << fir_json(applied) << ",\"ignored_profile_fields\":[";
        for (size_t i = 0; i < file.ignored_profile_fields.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << json(file.ignored_profile_fields[i]);
        }
        std::cout << "]}\n";
    } else {
        print_fir(applied);
        presentation::status(presentation::level::ok, "Loaded " + p[2] + "; FIR remains off. Enable with: och fir " + p[0] + " on", std::cout);
    }
    return 0;
}
std::array<uint32_t, 6> clock_chain_values(const std::string& text) {
    std::array<uint32_t, 6> out{};
    size_t start = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        auto end = text.find(',', start);
        if ((i < 5 && end == std::string::npos) || (i == 5 && end != std::string::npos))
            throw std::invalid_argument("clock chain must contain exactly six comma-separated Hz values");
        auto token = text.substr(start, end == std::string::npos ? end : end - start);
        out[i] = u32(token);
        if (!out[i]) throw std::invalid_argument("clock frequencies must be positive");
        start = end == std::string::npos ? text.size() : end + 1;
    }
    return out;
}
std::string clock_json(const och_clock_config& c) {
    std::ostringstream out; out << "{\"reference_hz\":" << c.reference_hz << ",\"rx_hz\":[";
    for (size_t i = 0; i < 6; ++i) { if (i) out << ','; out << c.rx_hz[i]; }
    out << "],\"tx_hz\":[";
    for (size_t i = 0; i < 6; ++i) { if (i) out << ','; out << c.tx_hz[i]; }
    out << "]}"; return out.str();
}
int run_clock(const options& opt) {
    allowed(opt, {"rx-chain", "tx-chain"}, true);
    const auto& p = opt.positional;
    bool status = p.empty() || (p.size() == 1 && p[0] == "status");
    bool rate = p.size() == 2 && (p[0] == "sample-rate" || p[0] == "rate");
    bool explicit_chain = p.size() == 1 && p[0] == "set";
    if (!status && !rate && !explicit_chain) throw std::invalid_argument("use clock status, clock sample-rate HZ, or clock set --rx-chain CSV --tx-chain CSV");
    if (!explicit_chain && (opt.has("rx-chain") || opt.has("tx-chain"))) throw std::invalid_argument("clock chains apply only to clock set");
    och_clock_config requested{}; requested.struct_size = sizeof(requested);
    uint32_t sample_rate = 0;
    if (rate) {
        sample_rate = static_cast<uint32_t>(integer(p[1], 61440000));
        if (!sample_rate) throw std::invalid_argument("sample rate must be positive");
    }
    if (explicit_chain) {
        if (!opt.has("rx-chain") || !opt.has("tx-chain")) throw std::invalid_argument("clock set requires both --rx-chain and --tx-chain");
        auto rx = clock_chain_values(opt.get("rx-chain")), tx = clock_chain_values(opt.get("tx-chain"));
        if (rx[0] != tx[0] || rx[5] != tx[5] || rx[5] > 61440000)
            throw std::invalid_argument("clock chains require the same BBPLL and RX/TX sample rate, at most 61.44M");
        std::copy(rx.begin(), rx.end(), requested.rx_hz); std::copy(tx.begin(), tx.end(), requested.tx_hz);
    }
    och::device d(opt.get("serial"));
    if (rate) d.set_sample_rate(sample_rate);
    if (explicit_chain) d.set_clock(requested);
    auto actual = d.clock();
    if (opt.has("json")) std::cout << clock_json(actual) << '\n';
    else presentation::clocks(actual, std::cout);
    if (rate) report_rate_adjustment(sample_rate, actual.rx_hz[5], opt.has("json"));
    return 0;
}
// --- CH32 bridge firmware ------------------------------------------------------
std::string firmware_version(uint32_t v) {
    return v ? std::to_string(v >> 16) + "." + std::to_string((v >> 8) & 255) + "." + std::to_string(v & 255) : "none";
}
const char* firmware_mode(uint32_t mode) {
    return mode == OCH_FW_MODE_APP ? "application" : mode == OCH_FW_MODE_BOOTLOADER ? "bootloader" : "legacy";
}
std::string firmware_json(const och_firmware_info& f) {
    std::ostringstream out;
    out << "{\"serial\":" << json(f.serial) << ",\"mode\":" << json(firmware_mode(f.mode));
    if (f.mode != OCH_FW_MODE_LEGACY)
        out << ",\"running_version\":" << json(firmware_version(f.running_version))
            << ",\"bootloader_version\":" << json(firmware_version(f.bootloader_version))
            << ",\"app_version\":" << json(firmware_version(f.app_version))
            << ",\"app_programmed\":" << ((f.app_status & OCH_FW_APP_PROGRAMMED) ? "true" : "false")
            << ",\"app_image_verified\":" << ((f.app_status & OCH_FW_APP_IMAGE_OK) ? "true" : "false")
            << ",\"build_id\":" << json(f.build_id);
    out << '}';
    return out.str();
}
void print_firmware(const och_firmware_info& f) {
    using presentation::tone;
    presentation::section("Bridge firmware (CH32)", std::cout);
    presentation::field("Mode", firmware_mode(f.mode), std::cout,
                        f.mode == OCH_FW_MODE_APP ? tone::good : tone::warn);
    if (f.mode == OCH_FW_MODE_LEGACY) {
        std::cout << '\n';
        presentation::note("Firmware before 1.0.0 has no version or USB bootloader; flash Merge.bin once with WCH-Link.", std::cout);
        return;
    }
    if (f.mode == OCH_FW_MODE_APP)
        presentation::field("Version", firmware_version(f.running_version) + "  (" + f.build_id + ")", std::cout, tone::accent);
    presentation::field("Bootloader", firmware_version(f.bootloader_version), std::cout);
    if (f.mode == OCH_FW_MODE_BOOTLOADER) {
        presentation::field("Installed application", firmware_version(f.app_version), std::cout);
        presentation::field("Application", (f.app_status & OCH_FW_APP_IMAGE_OK) ? "verified" :
                            (f.app_status & OCH_FW_APP_PROGRAMMED) ? "present, not verified" : "missing", std::cout,
                            (f.app_status & OCH_FW_APP_PROGRAMMED) ? tone::plain : tone::warn);
        std::cout << '\n';
        presentation::note("Leave with och firmware boot, or install with och firmware update FILE.", std::cout);
    }
}
void firmware_progress(void* user, uint32_t stage, uint32_t done, uint32_t total) {
    static const char* names[] = {"", "Entering bootloader", "Erasing", "Writing", "Verifying", "Restarting"};
    auto& last = *static_cast<uint32_t*>(user);
    if (stage == OCH_FW_STAGE_WRITE) {
        std::cout << "\r  " << names[stage] << "  " << (total ? done * 100ULL / total : 100) << "%  ("
                  << done << " / " << total << " bytes)" << std::flush;
        if (done == total) std::cout << '\n';
    } else if (stage != last && done == 0) {
        std::cout << "  " << names[stage] << "...\n" << std::flush;
    }
    last = stage;
}
int run_firmware(const options& opt) {
    const bool json_mode = opt.has("json");
    const auto& p = opt.positional;
    const std::string action = p.empty() ? "info" : p[0];
    const std::string serial_text = opt.get("serial");
    const char* serial = serial_text.empty() ? nullptr : serial_text.c_str();
    allowed(opt, {}, true);
    if ((action == "info" || action == "boot" || action == "enter") && p.size() > 1)
        throw std::invalid_argument("och firmware " + action + " takes no file");
    if ((action == "update" || action == "check") && p.size() != 2)
        throw std::invalid_argument("use och firmware " + action + " FILE.ocfw");
    if (action == "check" || action == "update") {
        och_firmware_image img{}; img.struct_size = sizeof img;
        och::check(och_firmware_read_image(p[1].c_str(), &img));
        if (action == "check") {
            if (json_mode) {
                std::cout << "{\"version\":" << json(firmware_version(img.version)) << ",\"image_size\":" << img.image_size
                          << ",\"image_crc32\":" << img.image_crc32 << ",\"build_unix\":" << img.build_unix
                          << ",\"build_id\":" << json(img.build_id) << "}\n";
            } else {
                presentation::section("Bridge firmware image", std::cout);
                presentation::field("Version", firmware_version(img.version), std::cout, presentation::tone::accent);
                presentation::field("Build", img.build_id, std::cout);
                presentation::field("Size", std::to_string(img.image_size) + " bytes", std::cout);
                presentation::field("CRC32", presentation::detail::hex(img.image_crc32, 8), std::cout);
            }
            return 0;
        }
        if (!json_mode) {
            presentation::section("Bridge firmware update", std::cout);
            presentation::field("Image", p[1] + "  (" + firmware_version(img.version) + ", " + img.build_id + ")", std::cout);
            std::cout << '\n';
            presentation::note("Do not unplug the device. If interrupted, it stays in the bootloader; run update again.", std::cout);
            std::cout << '\n';
        }
        uint32_t last = 0;
        och::check(och_firmware_update(serial, p[1].c_str(), json_mode ? nullptr : firmware_progress, &last));
    } else if (action == "boot") {
        och::check(och_firmware_start_app(serial));
    } else if (action == "enter") {
        och::check(och_firmware_enter_bootloader(serial));
    } else if (action != "info") {
        throw std::invalid_argument("use och firmware [info|check FILE|update FILE|boot|enter]");
    }
    och_firmware_info f{}; f.struct_size = sizeof f;
    och::check(och_firmware_query(serial, &f));
    if (json_mode) std::cout << firmware_json(f) << '\n';
    else {
        print_firmware(f);
        if (action == "update") {
            std::cout << '\n';
            presentation::status(presentation::level::ok, "Bridge firmware " + firmware_version(f.running_version) + " installed.", std::cout);
        }
    }
    return 0;
}
int execute(const options& opt) {
    bool json_mode = opt.has("json");
    if (opt.command == "help" || opt.has("help")) {
        if (json_mode) std::cout << "{\"help\":" << json(help_text()) << "}\n";
        else print_help(std::cout);
        return 0;
    }
    if (opt.command == "version") {
        allowed(opt, {});
        if (json_mode) std::cout << "{\"name\":\"OmniCore Host\",\"version\":" << json(och_version()) << ",\"abi\":" << OCH_ABI_VERSION << "}\n";
        else {
            presentation::section("OmniCore Host", std::cout);
            presentation::field("Version", och_version(), std::cout, presentation::tone::accent);
            presentation::field("Public C/C++ ABI", OCH_ABI_VERSION, std::cout);
#ifdef _WIN32
            presentation::field("Platform", "Windows x64, libusbK", std::cout);
#else
            presentation::field("Platform", "Linux, libusb-1.0", std::cout);
#endif
            presentation::field("Purpose", "OmniCoreSDR control and IQ streaming", std::cout);
            std::cout << '\n';
            presentation::note("Next: och find  |  och info  |  och help", std::cout);
        }
        return 0;
    }
    if (opt.command == "find") {
        allowed(opt, {}); auto devices = och::find();
        std::vector<och_device_info> selected;
        for (const auto& i : devices)
            if (!opt.has("serial") || opt.get("serial") == i.serial) selected.push_back(i);
        if (json_mode) {
            std::cout << '[';
            for (size_t n = 0; n < selected.size(); ++n) { if (n) std::cout << ','; print_info(selected[n], true); }
            std::cout << "]\n";
        } else presentation::devices(selected, std::cout);
        return 0;
    }
    if (opt.command == "rx" || opt.command == "tx" || opt.command == "bench") return run_stream(opt);
    if (opt.command == "fir") return run_fir(opt);
    if (opt.command == "clock") return run_clock(opt);
    if (opt.command == "commands") {
        allowed(opt, {});
        if (json_mode) std::cout << commands::catalog_json() << '\n';
        else {
            print_catalog(std::cout);
        }
        return 0;
    }
    if (opt.command == "info") {
        allowed(opt, {}); och::device d(opt.get("serial"));
        auto info = d.info(); auto power = d.power_status();
        och_firmware_info bridge{}; bridge.struct_size = sizeof bridge;
        och::check(och_get_firmware_info(d.native_handle(), &bridge));
        och_time_status time{}; time.struct_size = sizeof time;
        const bool have_time = och_get_time(d.native_handle(), &time) == OCH_OK;
        print_info(info, json_mode, &power, &bridge, have_time ? &time : nullptr);
        if (json_mode) std::cout << '\n';
        return 0;
    }
    if (opt.command == "firmware") return run_firmware(opt);
    if (opt.command == "rssi") {
        // och rssi on|off switches reporting (off after power-up); otherwise read.
        const bool toggle = opt.positional.size() == 1 && (opt.positional[0] == "on" || opt.positional[0] == "off");
        if (!opt.positional.empty() && !toggle) throw std::invalid_argument("use och rssi, och rssi on or och rssi off");
        allowed(opt, toggle ? std::set<std::string>{} : std::set<std::string>{"count", "interval"}, toggle);
        const auto count = integer(opt.get("count", "1"), 100000000);
        const double interval = number(opt.get("interval", "0.5"));
        if (!(interval >= 0.01 && interval <= 3600)) throw std::invalid_argument("--interval must be 0.01..3600 seconds");
        och::device d(opt.get("serial"));
        if (toggle) {
            d.set_rx_rssi_enabled(opt.positional[0] == "on");
            const bool on = d.rx_rssi().enabled != 0;
            if (json_mode) std::cout << "{\"rssi_enabled\":" << (on ? "true" : "false") << "}\n";
            else presentation::status(presentation::level::ok, on ? "RSSI reporting on." : "RSSI reporting off.", std::cout);
            return 0;
        }
        const auto began = std::chrono::steady_clock::now();
        for (uint64_t n = 0; (!count || n < count) && !interrupted; ++n) {
            if (n) {
                const auto due = began + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(interval * static_cast<double>(n)));
                while (!interrupted && std::chrono::steady_clock::now() < due)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (interrupted) break;
            }
            const auto r = d.rx_rssi();
            const auto t = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            if (!r.enabled) {
                if (json_mode) std::cout << "{\"type\":\"rx_rssi\",\"enabled\":false,\"rx1_db\":null,\"rx2_db\":null}\n";
                else {
                    presentation::status(presentation::level::warn, "RSSI reporting is off (default after power-up).", std::cout);
                    presentation::note("Enable it with: och rssi on", std::cout);
                }
                return 1;
            }
            auto db = [&](unsigned i) {
                std::ostringstream s; s.imbue(std::locale::classic());
                if (r.valid[i]) s << std::fixed << std::setprecision(2) << r.rssi_db[i]; else s << (json_mode ? "null" : "--");
                return s.str();
            };
            if (json_mode) {
                std::cout << "{\"type\":\"rx_rssi\",\"enabled\":true,\"t_s\":" << std::fixed << std::setprecision(3) << t
                          << ",\"usb_rx_mask\":" << r.usb_rx_mask << ",\"rx1_db\":" << db(0) << ",\"rx2_db\":" << db(1)
                          << ",\"unit\":\"dB_relative\",\"calibrated\":false}\n" << std::flush;
            } else if (count == 1) {
                presentation::section("RX RSSI (relative dB, not calibrated power)", std::cout);
                presentation::field_text("RX1", r.valid[0] ? db(0) + " dB" : "not receiving", std::cout);
                presentation::field_text("RX2", r.valid[1] ? db(1) + " dB" : "not receiving", std::cout);
            } else {
                std::cout << std::fixed << std::setprecision(2) << std::setw(8) << t << " s  RX1 " << std::setw(8) << db(0)
                          << " dB  RX2 " << std::setw(8) << db(1) << " dB\n" << std::flush;
            }
        }
        return interrupted ? 130 : 0;
    }
    if (opt.command == "power") {
        allowed(opt, {}, true);
        if (!opt.positional.empty() && (opt.positional.size() != 1 || opt.positional[0] != "status"))
            throw std::invalid_argument("use och power or och power status");
        och::device d(opt.get("serial")); auto power = d.power_status();
        if (json_mode) std::cout << power_json(power) << '\n';
        else print_power(power);
        return 0;
    }
    if (opt.command == "config") {
        allowed(opt, rf_options);
        bool mutation = false; for (const auto& value : opt.values) if (value.first != "serial") mutation = true;
        auto c = och::default_config();
        if (mutation) apply_rf(opt, c);
        och::device d(opt.get("serial"));
        if (mutation) {
            d.configure(c);
            auto applied = d.config();
            if (json_mode) std::cout << config_json(applied) << '\n';
            else {
                presentation::config(applied, std::cout);
                presentation::status(presentation::level::ok, "Configuration applied; TX was not started.", std::cout);
            }
            report_rate_adjustment(c.sample_rate, applied.sample_rate, json_mode);
        }
        else {
            const auto raw_reply = d.command("system_status?");
            const auto reply = monitor_reply(d,"system_status?",raw_reply);
            if (json_mode) std::cout << "{\"reply\":" << json(reply) << "}\n";
            else presentation::reply("System status", reply, std::cout);
        }
        return 0;
    }
    if (opt.command == "get" || opt.command == "set" || opt.command == "action" ||
        opt.command == "do" || opt.command == "selftest") {
        allowed(opt, {}, opt.command != "selftest");
        std::string cmd, canonical;
        const bool action = opt.command == "action" || opt.command == "do" || opt.command == "selftest";
        if (opt.command == "selftest") cmd = "digital_self_test!";
        else {
            if (opt.positional.empty()) throw std::invalid_argument("command name is required; use och commands");
            cmd = opt.positional[0];
            if (cmd.empty()) throw std::invalid_argument("empty command name");
            if (commands::known(cmd)) {
                canonical = cmd;
                const auto operation = action ? commands::Operation::action :
                    opt.command == "get" ? commands::Operation::query : commands::Operation::set;
                cmd = commands::translate(canonical,
                    std::vector<std::string>(opt.positional.begin() + 1, opt.positional.end()), operation);
            } else if (action || (cmd.find('.') != std::string::npos && cmd.find('=') == std::string::npos)) {
                throw std::invalid_argument("unknown canonical command: " + cmd + "; use och commands");
            } else if (opt.command == "get") {
                if (opt.positional.size() != 1 || cmd.find('=') != std::string::npos || cmd.find('!') != std::string::npos)
                    throw std::invalid_argument("get accepts one query name only");
                if (cmd.back() != '?') cmd += '?';
            } else if (opt.positional.size() > 1) {
                if (cmd.find('=') == std::string::npos) cmd += '=';
                for (size_t i = 1; i < opt.positional.size(); ++i) { if (i > 1) cmd += ' '; cmd += opt.positional[i]; }
            } else if (cmd.find('=') == std::string::npos && cmd.back() != '!')
                throw std::invalid_argument("set needs values, an '=' command, or a '!' action");
            if (cmd.find('\n') != std::string::npos || cmd.find('\r') != std::string::npos)
                throw std::invalid_argument("one SDK command per invocation; newlines are forbidden");
        }
        // Clock/FIR setters run checked digital calibration and can take longer
        // than a scalar query. Preserve the same budget for legacy ! actions.
        const bool long_control = action || opt.command == "set";
        och::device d(opt.get("serial"));
        std::string raw_reply;
        uint32_t requested_rate = 0, applied_rate = 0;
        if(canonical=="rgb.enabled" || canonical=="rgb.status") {
            if(opt.command=="set")d.set_rgb_enabled(cmd.back()=='1');
            const auto status=d.rgb_status();
            raw_reply=canonical=="rgb.status"?board_view::rgb(status):
                "rgb_enable="+std::to_string(status.enabled)+
                " brightness_pct="+std::to_string(status.brightness_pct)+"\n";
        }else if(canonical=="fan.enabled" || canonical=="fan.status") {
            if(opt.command=="set")d.set_fan_enabled(cmd.back()=='1');
            const auto status=d.fan_status();
            raw_reply=canonical=="fan.status"?board_view::fan(status):
                "fan_enable="+std::to_string(status.enabled)+" mode="+(status.enabled?"auto\n":"off\n");
        }else if(canonical=="rx.rssi") {
            raw_reply=board_view::rssi(d.rx_rssi());
        }else if(canonical=="rx.rssi-enabled") {
            if(opt.command=="set")d.set_rx_rssi_enabled(cmd.back()=='1');
            raw_reply="rssi_enable="+std::to_string(d.rx_rssi().enabled)+"\n";
        }else if(canonical=="core.temperature") {
            raw_reply=board_view::temperature(d.core_temperature());
        }else if(canonical=="pps.input-enabled" || canonical=="pps.status") {
            if(opt.command=="set")d.set_pps_enabled(cmd.back()=='1');
            else (void)d.pps_status();
            raw_reply=d.command(canonical=="pps.status"?"pps_status?":"pps_input_en?");
        }else if(canonical=="clock.vcxo-status") {
            (void)d.vcxo_status(); // capability and consistency check
            raw_reply=d.command("vcxo_status?");
        }else if(canonical=="rx.dc-correction" || canonical=="rx.iq-correction") {
            const bool dc=canonical=="rx.dc-correction";
            if(opt.command=="set") {
                // translate has already checked Boolean syntax/range. Use the
                // typed setter for capability checking and verified readback.
                const bool enabled=cmd.back()=='1';
                if(dc)d.set_rx_dc_correction(enabled);else d.set_rx_iq_correction(enabled);
            }else (void)d.rx_correction();
            // Keep the raw diagnostics and the ordinary command JSON shape.
            raw_reply=d.command(dc?"rx_dc_correction?":"rx_iq_correction?");
        }else if(canonical=="rx.lo-source"||canonical=="tx.lo-source") {
            if(opt.command=="set") {
                const auto source=cmd.back()=='1'?OCH_SOURCE_EXTERNAL:OCH_SOURCE_INTERNAL;
                if(canonical=="rx.lo-source")d.set_rx_lo_source(source);else d.set_tx_lo_source(source);
            }else (void)d.lo_source();
            raw_reply=d.command(canonical=="rx.lo-source"?"rx_lo_source?":"tx_lo_source?");
        }else if(canonical=="clock.ref-source") {
            if(opt.command=="set")d.set_reference_source(cmd.back()=='1'?OCH_SOURCE_EXTERNAL:OCH_SOURCE_INTERNAL);
            else (void)d.reference_status();
            raw_reply=d.command("clock_ref_source?");
        }else if(canonical=="clock.ref-status"||canonical=="clock.ref-test") {
            const auto before=d.reference_status(); // Preserve diagnostic unknown state in status queries.
            const auto healthy=[](const och_reference_status& s){return s.initialized&&s.source_known&&s.gpio_verified&&!s.last_error;};
            if(canonical=="clock.ref-test"&&!healthy(before))
                throw std::runtime_error("reference source is unknown or failed; use och set clock.ref-source internal before self-test");
            raw_reply=d.command(cmd,long_control?15000:5000);
            if(canonical=="clock.ref-test") {
                const auto after=d.reference_status();
                if(!healthy(after)||after.source!=before.source||after.led_mode!=before.source)
                    throw std::runtime_error("ADF4002 self-test did not restore a healthy reference source");
            }
        }else if(opt.command=="set" && (canonical=="rx.sample-rate"||canonical=="tx.sample-rate"||canonical=="clock.sample-rate")) {
            // Typed setter: applies the nearest accepted rate if the SDK has
            // no exact clock plan. translate has already range-checked it.
            requested_rate=static_cast<uint32_t>(std::stoul(cmd.substr(cmd.find('=')+1)));
            d.set_sample_rate(requested_rate);
            applied_rate=d.clock().rx_hz[5];
            raw_reply="clock_sample_rate="+std::to_string(applied_rate)+"\n";
        }else raw_reply=d.command(cmd,long_control?15000:5000);
        const auto view = monitor_reply(d,cmd,raw_reply);
        auto reply = canonical.empty() ? view : commands::canonical_reply(canonical, view);
        if (json_mode && !canonical.empty())
            std::cout << "{\"command\":" << json(canonical) << ",\"sdk_command\":" << json(cmd)
                      << ",\"reply\":" << json(reply) << ",\"raw_reply\":" << json(raw_reply) << "}\n";
        else if (json_mode) std::cout << "{\"command\":" << json(cmd) << ",\"reply\":" << json(reply) << "}\n";
        else presentation::reply(canonical.empty() ? cmd : canonical, reply, std::cout);
        report_rate_adjustment(requested_rate, applied_rate, json_mode);
        return 0;
    }
    throw std::invalid_argument("unknown command: " + opt.command + "; use och help");
}
} // namespace
int main(int argc, char** argv) {
#if defined(_WIN32) && !defined(OCH_CLI_TEST)
    // Use the Unicode command line, independent of the Windows ANSI code page.
    // File paths then have the UTF-8 representation expected by u8path().
    std::vector<std::string> utf8_args;
    std::vector<char*> utf8_ptrs;
    int wide_count = 0;
    LPWSTR* wide_args = CommandLineToArgvW(GetCommandLineW(), &wide_count);
    if (wide_args) {
        utf8_args.reserve(wide_count);
        for (int i = 0; i < wide_count; ++i) {
            int count = WideCharToMultiByte(CP_UTF8, 0, wide_args[i], -1, nullptr, 0, nullptr, nullptr);
            std::string value(static_cast<size_t>(count), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide_args[i], -1, value.data(), count, nullptr, nullptr);
            if (!value.empty()) value.pop_back();
            utf8_args.push_back(std::move(value));
        }
        LocalFree(wide_args);
        for (auto& value : utf8_args) utf8_ptrs.push_back(value.data());
        argc = static_cast<int>(utf8_ptrs.size()); argv = utf8_ptrs.data();
    }
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::setlocale(LC_NUMERIC, "C");
#if defined(__linux__) && !defined(OCH_CLI_TEST)
    std::setlocale(LC_CTYPE, "");
#endif
    std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
    bool json_mode = false, no_color = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--") break;
        if (arg == "--json") json_mode = true;
        if (arg == "--no-color") no_color = true;
    }
    if (const char* env = std::getenv("NO_COLOR")) if (*env) no_color = true;
    if (!json_mode) {
        presentation::configure(std::cout, terminal_style(true, no_color));
        presentation::configure(std::cerr, terminal_style(false, no_color));
    }
    try { return execute(parse(argc, argv)); }
    catch (const std::invalid_argument& e) {
        if (json_mode) std::cerr << "{\"error\":" << json(e.what()) << ",\"exit_code\":2}\n";
        else {
            std::cerr << '\n';
            presentation::status(presentation::level::fail, std::string("Invalid argument: ") + e.what(), std::cerr);
            presentation::note("Exit code 2. See och help, or och commands for control names.", std::cerr);
        }
        return 2;
    } catch (const std::exception& e) {
        if (json_mode) std::cerr << "{\"error\":" << json(e.what()) << ",\"exit_code\":1}\n";
        else {
            std::cerr << '\n';
            presentation::status(presentation::level::fail, e.what(), std::cerr);
            presentation::note("Exit code 1. The command did not complete.", std::cerr);
        }
        return 1;
    }
}
