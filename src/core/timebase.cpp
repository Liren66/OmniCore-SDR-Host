// Hardware time base, timed RX commands and FPGA timing events
// (FPGA CH32H417_IP 1.3 + SDK time commands; framed transport only).
#include "internal.hpp"
#include <algorithm>
#include <charconv>
#include <cstdint>
using namespace och::core;

namespace {
    std::string field(const std::string &s, const std::string &name) {
        size_t at = 0;
        while ((at = s.find(name + "=", at)) != std::string::npos) {
            if (!at || s[at - 1] == ' ' || s[at - 1] == '\t' || s[at - 1] == '\r' || s[at - 1] == '\n') {
                at += name.size() + 1;
                return s.substr(at, s.find_first_of(" \t\r\n", at) - at);
            }
            ++at;
        }
        throw Failure(OCH_PROTOCOL, "SDK reply missing " + name);
    }
    uint64_t u64(const std::string &s) {
        uint64_t v = 0;
        auto p = std::from_chars(s.data(), s.data() + s.size(), v);
        require(p.ec == std::errc() && p.ptr == s.data() + s.size(), OCH_PROTOCOL, "invalid integer in SDK reply");
        return v;
    }
    uint32_t source_of(const std::string &s) {
        return s == "external"   ? OCH_TIME_SOURCE_EXTERNAL
               : s == "internal" ? OCH_TIME_SOURCE_INTERNAL
               : s == "sync"     ? OCH_TIME_SOURCE_SYNC
                                 : OCH_TIME_SOURCE_NONE;
    }
}

och_time_status Device::get_time() {
    och_time_status t{};
    t.struct_size = sizeof t;
    if (!supports_capability("hw_time")) return t;
    const auto r = command("time_status?");
    t.capable = 1;
    t.valid = uint32_t(u64(field(r, "valid")));
    t.armed = uint32_t(u64(field(r, "armed")));
    t.source = source_of(field(r, "source"));
    t.ref_present = uint32_t(u64(field(r, "ref40_present")));
    t.pps_count = uint32_t(u64(field(r, "pps_count")));
    t.tick_rate = uint32_t(u64(field(r, "tick_rate")));
    t.now = u64(field(r, "now"));
    t.last_pps = u64(field(r, "last_pps"));
    t.framing = uint32_t(u64(field(r, "framing")));
    t.rx_timed = uint32_t(u64(field(r, "rx_timed")));
    t.fpga_version = uint32_t(std::stoul(field(r, "version"), nullptr, 16));
    return t;
}

// EVT tseq=<n> type=<name> src=fpga_time code=<c> block=<seq> time=<ticks> lost_before=<0|1>
// EVT tseq=<n> type=time_invalidated src=sdk old_rate=<hz> new_rate=<hz>
// Firmware after v2026.11.07 coalesces tx_underflow / rx_overflow_gap bursts:
// the first event as above, then at most one line per 250 ms with
// "count=<n> first_block=<b> first_time=<ticks>" appended (n events, block and
// time of the last one). Lines without count stand for one event.
bool och::core::timed_event(Device &d, const std::string &s) {
    // EVT tseq=<n> type=sync_in src=sync count=<total> new=<n> interval=<cycles>
    if (s.find(" src=sync") != std::string::npos && s.find("type=sync_in") != std::string::npos) {
        uint32_t fresh = 1;
        try {
            fresh = uint32_t(std::max<uint64_t>(1, std::min<uint64_t>(u64(field(s, "new")), UINT32_MAX)));
        } catch (...) {}
        d.emit(OCH_EVENT_SYNC_IN, s, d.rx ? d.rx->epoch : 0, 0, 0, 0, fresh);
        return true;
    }
    if (s.find(" src=fpga_time") == std::string::npos && s.find("type=time_invalidated") == std::string::npos)
        return false;
    uint32_t type = OCH_EVENT_DEVICE;
    std::string name;
    try {
        name = field(s, "type");
    } catch (...) {
        return false;
    }
    if (name == "tx_burst_ack")
        type = OCH_EVENT_TX_BURST_ACK;
    else if (name == "tx_late")
        type = OCH_EVENT_TX_LATE;
    else if (name == "tx_underflow")
        type = OCH_EVENT_TX_UNDERFLOW;
    else if (name == "tx_seq_error" || name == "tx_header_error")
        type = OCH_EVENT_TX_ERROR;
    else if (name == "rx_overflow_gap")
        type = OCH_EVENT_RX_GAP;
    else if (name == "rx_late_command" || name == "rx_command_ignored")
        type = OCH_EVENT_RX_LATE;
    else if (name == "time_invalidated")
        type = OCH_EVENT_TIME_INVALIDATED;
    uint64_t t = 0;
    uint32_t code = 0, block = 0, count = 1;
    try {
        t = u64(field(s, "time"));
    } catch (...) {}
    try {
        code = uint32_t(u64(field(s, "code")));
    } catch (...) {}
    try {
        block = uint32_t(u64(field(s, "block")));
    } catch (...) {}
    if (s.find(" count=") != std::string::npos) try {
            count = uint32_t(std::max<uint64_t>(1, std::min<uint64_t>(u64(field(s, "count")), UINT32_MAX)));
        } catch (...) {}
    if (type == OCH_EVENT_TX_UNDERFLOW) d.underflows += count;
    d.emit(type, s, d.rx ? d.rx->epoch : 0, t, code, block, count);
    return true;
}
