#include "internal.hpp"
#include <charconv>
#include <chrono>
#include <sstream>
#include <thread>
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
    int64_t integer(const std::string &s) {
        int64_t v = 0;
        auto p = std::from_chars(s.data(), s.data() + s.size(), v);
        require(p.ec == std::errc() && p.ptr == s.data() + s.size(), OCH_PROTOCOL, "invalid integer in SDK reply");
        return v;
    }
    double real(const std::string &s) {
        std::istringstream in(s);
        in.imbue(std::locale::classic());
        double v = 0;
        char extra = 0;
        require(static_cast<bool>(in >> v) && !(in >> extra) && std::isfinite(v), OCH_PROTOCOL,
                "invalid number in SDK reply");
        return v;
    }
    uint32_t u32(const std::string &s) {
        auto v = integer(s);
        require(v >= 0 && v <= UINT32_MAX, OCH_PROTOCOL, "SDK integer out of range");
        return static_cast<uint32_t>(v);
    }
    uint32_t token() {
        static std::atomic<uint32_t> n{static_cast<uint32_t>(now_ns() / 1000000)};
        auto v = ++n;
        if (!v) v = ++n;
        return v;
    }
    uint32_t crc(const int16_t *p, unsigned n) {
        uint32_t c = 0xffffffffU;
        for (unsigned i = 0; i < n; ++i)
            for (unsigned byte = 0; byte < 2; ++byte) {
                c ^= (static_cast<uint16_t>(p[i]) >> (8 * byte)) & 255U;
                for (unsigned bit = 0; bit < 8; ++bit) c = (c >> 1) ^ ((0U - (c & 1U)) & 0xedb88320U);
            }
        return ~c;
    }
    void valid(const och_fir_config &c) {
        require(c.struct_size == sizeof c && c.direction <= OCH_TX && c.channel_mask == 3, OCH_INVALID,
                "FIR ABI/direction/channel mask invalid; both physical channels are required");
        require(c.factor == 1 || c.factor == 2 || c.factor == 4, OCH_INVALID, "FIR factor must be 1, 2 or 4");
        require(c.tap_count >= 16 && c.tap_count <= 128 && c.tap_count % 16 == 0, OCH_INVALID,
                "FIR needs 16..128 taps in multiples of 16");
        require(c.enabled <= 1, OCH_INVALID, "FIR enabled must be 0 or 1");
        require(c.direction == OCH_RX ? (c.gain_db == -12 || c.gain_db == -6 || c.gain_db == 0 || c.gain_db == 6)
                                      : (c.gain_db == -6 || c.gain_db == 0),
                OCH_INVALID, "unsupported FIR gain");
        require(!(c.direction == OCH_TX && c.factor == 1 && c.tap_count > 64), OCH_INVALID,
                "TX factor 1 permits at most 64 taps");
    }
    std::string csv(const uint32_t *p) {
        std::string s;
        for (unsigned i = 0; i < 6; ++i) {
            if (i) s += ',';
            s += std::to_string(p[i]);
        }
        return s;
    }
    // Mirrors ad9361_calculate_rf_clock_chain() of the ADI driver in the SDK
    // (rate_governor 1, rx_eq_2tx 0): the BBPLL rate it targets for this sample
    // rate and FIR decimation/interpolation, or 0 when no divider chain exists.
    uint64_t planned_bbpll(uint32_t rate, uint32_t rx_intdec, uint32_t tx_intdec) {
        static const uint32_t dividers[7][4] = {{12, 3, 2, 2}, {8, 2, 2, 2}, {6, 3, 1, 2}, {4, 2, 2, 1},
                                                {3, 3, 1, 1},  {2, 2, 1, 1}, {1, 1, 1, 1}};
        constexpr uint64_t min_adc = 25000000, max_adc = 640000000, max_dac = 320000000, max_bbpll = 1430000000;
        if (!rate || rate > 61440000) return 0;
        const uint64_t clkrf = uint64_t(rate) * rx_intdec, clktf = uint64_t(rate) * tx_intdec;
        int governor = 1;
        for (;;) {
            bool recursion = true;
            if (governor == 1 && clkrf * 8 < min_adc) {
                recursion = false;
                governor = 0;
            }
            uint64_t adc = 0;
            int index_rx = -1, index_tx = -1;
            for (int i = governor; i < 7; ++i) {
                adc = clkrf * dividers[i][0];
                const uint64_t dac = clktf * dividers[i][0];
                if (adc > max_adc || adc < min_adc) continue;
                const int64_t tmp = dac > adc ? -int64_t(dac / adc) : int64_t(adc / dac);
                index_rx = i;
                if (adc <= max_dac)
                    index_tx = int(i - (tmp == 1 ? 0 : tmp));
                else
                    index_tx =
                            (i == 4 && tmp >= 0) ? 7 : int(i + ((i == 5 && tmp >= 0) ? 1 : 2) - (tmp == 1 ? 0 : tmp));
                break;
            }
            if (index_rx < 0 || index_rx > 6 || index_tx < 0 || index_tx > 6) {
                if (governor < 7 && recursion) {
                    ++governor;
                    continue;
                }
                return 0;
            }
            uint64_t bbpll = 0;
            for (uint32_t div = 64;; div >>= 1) {
                bbpll = adc * div;
                if (bbpll <= max_bbpll || div <= 2) break;
            }
            return bbpll;
        }
    }
    // ad9361_bbpll_round_rate() picks the nearest fractional word (modulus
    // 2088960) and recalc_rate() floors; the SDK accepts the plan only when the
    // programmed BBPLL reads back as exactly the requested rate.
    bool bbpll_exact(uint64_t bbpll, uint64_t reference) {
        constexpr uint64_t modulus = 2088960;
        if (!reference || bbpll < 715000000 || bbpll > 1430000000) return false;
        const uint64_t integer = bbpll / reference, fract = ((bbpll % reference) * modulus + reference / 2) / reference;
        return reference * fract / modulus + reference * integer == bbpll;
    }
    void clocks(const std::string &s, uint32_t *out) {
        size_t at = 0;
        for (unsigned i = 0; i < 6; ++i) {
            auto end = s.find(',', at);
            require((end != std::string::npos) == (i < 5), OCH_PROTOCOL, "SDK clock chain must contain six rates");
            out[i] = u32(s.substr(at, end == std::string::npos ? end : end - at));
            require(out[i] != 0, OCH_PROTOCOL, "SDK clock chain contains zero");
            at = end + 1;
        }
    }
    void unchanged_rate(const och_clock_config &a, const och_clock_config &b) {
        require(a.rx_hz[5] == b.rx_hz[5] && a.tx_hz[5] == b.tx_hz[5], OCH_PROTOCOL,
                "FIR changed sample rate unexpectedly; configure again before streaming");
    }
    och_rx_correction_config read_rx_correction(Device &d) {
        och_rx_correction_config c{};
        c.struct_size = sizeof c;
        c.dc_enabled = u32(field(d.command("rx_dc_correction?"), "rx_dc_correction"));
        c.iq_enabled = u32(field(d.command("rx_iq_correction?"), "rx_iq_correction"));
        require(c.dc_enabled <= 1 && c.iq_enabled <= 1, OCH_PROTOCOL, "SDK RX correction states must be 0 or 1");
        return c;
    }
    och_lo_source_config read_lo_source(Device &d) {
        och_lo_source_config c{};
        c.struct_size = sizeof c;
        for (unsigned dir = 0; dir < 2; ++dir) {
            const std::string name = dir ? "tx_lo_source" : "rx_lo_source";
            const auto text = d.command(name + "?");
            const auto source = u32(field(text, name));
            const auto hz = integer(field(text, "frequency_hz"));
            require(source <= 1 && hz >= 70000000 && hz <= (source ? 4000000000LL : 6000000000LL), OCH_PROTOCOL,
                    "SDK LO source or nominal frequency is out of range");
            require(u32(field(text, "verified")) == 1, OCH_PROTOCOL, "SDK LO source readback is not verified");
            (dir ? c.tx_source : c.rx_source) = source;
            (dir ? c.tx_frequency : c.rx_frequency) = static_cast<uint64_t>(hz);
        }
        return c;
    }
    och_reference_status read_reference_status(Device &d) {
        const auto text = d.command("adf4002_status?");
        och_reference_status c{};
        c.struct_size = sizeof c;
        c.source = u32(field(text, "source"));
        c.initialized = u32(field(text, "initialized"));
        c.gpio_verified = u32(field(text, "gpio_verified"));
        c.chip_verified = u32(field(text, "chip_verified"));
        c.lock_known = u32(field(text, "lock_known"));
        const auto locked = integer(field(text, "locked"));
        c.led_known = u32(field(text, "led_known"));
        const auto led = integer(field(text, "led_on"));
        c.led_mode = u32(field(text, "led_mode"));
        c.reference_hz = u32(field(text, "reference_hz"));
        c.oscillator_hz = u32(field(text, "oscillator_hz"));
        c.r_counter = u32(field(text, "r_counter"));
        c.n_counter = u32(field(text, "n_counter"));
        c.source_known = u32(field(text, "source_known"));
        const auto last_error = integer(field(text, "last_error"));
        require(last_error >= INT32_MIN && last_error <= INT32_MAX, OCH_PROTOCOL,
                "SDK reference last_error is out of range");
        c.last_error = static_cast<int32_t>(last_error);
        require(c.source <= 1 && c.source_known <= 1 && c.initialized <= 1 && c.gpio_verified <= 1 &&
                        c.chip_verified <= 1 && c.lock_known <= 1 && c.led_known <= 1 && c.led_mode <= 1,
                OCH_PROTOCOL, "SDK reference status contains invalid flags");
        require(!c.source_known || (c.initialized && c.gpio_verified && !c.last_error), OCH_PROTOCOL,
                "SDK reference source_known contradicts its error/initialization status");
        require((c.lock_known ? (locked == 0 || locked == 1) : locked == -1) &&
                        (c.led_known ? (led == 0 || led == 1) : led == -1),
                OCH_PROTOCOL, "SDK reference status must preserve unknown lock/LED observations");
        require(c.reference_hz == 10000000 && c.oscillator_hz == 40000000 && c.r_counter > 0 && c.r_counter <= 16383 &&
                        c.n_counter > 0 && c.n_counter <= 8191,
                OCH_PROTOCOL, "SDK ADF4002 reference/divider status is invalid");
        c.locked = static_cast<int32_t>(locked);
        c.led_on = static_cast<int32_t>(led);
        return c;
    }
    bool reference_healthy(const och_reference_status &c) {
        return c.initialized && c.source_known && c.gpio_verified && !c.last_error;
    }
    och_pps_status read_pps_status(Device &d) {
        const auto text = d.command("pps_status?");
        och_pps_status c{};
        c.struct_size = sizeof c;
        c.available = u32(field(text, "available"));
        c.enabled = u32(field(text, "enabled"));
        c.signal_present = u32(field(text, "signal_present"));
        c.frequency_valid = u32(field(text, "frequency_valid"));
        c.nominal_hz = u32(field(text, "nominal_hz"));
        c.measured_hz = u32(field(text, "measured_hz"));
        c.pulse_count = u32(field(text, "pulse_count"));
        c.invalid_count = u32(field(text, "invalid_count"));
        c.age_ms = u32(field(text, "age_ms"));
        c.raw_counter = u32(field(text, "raw_counter"));
        c.hardware_timeout = u32(field(text, "hardware_timeout"));
        const char *states[] = {"disabled", "waiting", "valid", "no_signal", "invalid_interval", "clock_unavailable"};
        const auto state = field(text, "state");
        c.state = 6;
        for (unsigned i = 0; i < 6; ++i)
            if (state == states[i]) c.state = i;
        require(c.available == 1 && c.enabled <= 1 && c.signal_present <= 1 && c.frequency_valid <= 1 &&
                        c.hardware_timeout <= 1 && c.state < 6,
                OCH_PROTOCOL, "invalid PPS status flags/state");
        require(field(text, "monitor_only") == "1" && field(text, "adjustment") == "0" &&
                        field(text, "clock") == "DATA_CLK" && field(text, "expected_pps_hz") == "1",
                OCH_PROTOCOL, "PPS status is not the supported 1 Hz DATA_CLK monitor");
        require((c.state == OCH_PPS_DISABLED) == !c.enabled && (c.state == OCH_PPS_VALID) == bool(c.frequency_valid),
                OCH_PROTOCOL, "inconsistent PPS enable/valid state");
        require(c.nominal_hz <= 245760000U && (!c.enabled || ((c.state == OCH_PPS_CLOCK_UNAVAILABLE) == !c.nominal_hz)),
                OCH_PROTOCOL, "invalid PPS nominal clock");
        require(!c.signal_present || (c.enabled && c.age_ms <= 1500 && c.nominal_hz), OCH_PROTOCOL,
                "invalid PPS signal age");
        require((c.state != OCH_PPS_NO_SIGNAL && c.state != OCH_PPS_DISABLED && c.state != OCH_PPS_CLOCK_UNAVAILABLE) ||
                        !c.signal_present,
                OCH_PROTOCOL, "PPS signal contradicts state");
        require(c.state != OCH_PPS_INVALID_INTERVAL || c.signal_present, OCH_PROTOCOL, "invalid PPS interval state");
        if (c.frequency_valid) {
            require(c.signal_present && c.nominal_hz && c.measured_hz && c.raw_counter == c.measured_hz - 1 &&
                            uint64_t(c.measured_hz) * 10 >= uint64_t(c.nominal_hz) * 9 &&
                            uint64_t(c.measured_hz) * 10 <= uint64_t(c.nominal_hz) * 11,
                    OCH_PROTOCOL, "invalid PPS frequency measurement");
            c.error_ppm = (double(c.measured_hz) / c.nominal_hz - 1.0) * 1e6;
            std::istringstream value(field(text, "error_ppm"));
            value.imbue(std::locale::classic());
            double reported = 0;
            value >> reported;
            require(value && value.eof() && std::isfinite(reported) && std::abs(reported - c.error_ppm) < 0.00001,
                    OCH_PROTOCOL, "inconsistent PPS frequency error");
        } else
            require(!c.measured_hz && !c.raw_counter && field(text, "error_ppm") == "unknown", OCH_PROTOCOL,
                    "invalid/stale PPS measurement must remain unknown");
        c.lock_known = c.enabled && c.frequency_valid && c.signal_present && !c.hardware_timeout;
        c.locked = -1;
        if (c.lock_known) {
            // Compare integer counts: avoid rounding a borderline ppm value into lock.
            const uint64_t difference =
                    c.measured_hz > c.nominal_hz ? c.measured_hz - c.nominal_hz : c.nominal_hz - c.measured_hz;
            c.locked = difference * 1000000ULL <= uint64_t(c.nominal_hz) * OCH_PPS_LOCK_TOLERANCE_PPM;
        }
        return c;
    }
    och_vcxo_status read_vcxo_status(Device &d) {
        const auto text = d.command("vcxo_status?");
        och_vcxo_status c{};
        c.struct_size = sizeof c;
        c.requested_mode = u32(field(text, "requested_mode"));
        c.applied_mode = u32(field(text, "applied_mode"));
        c.pending = u32(field(text, "pending"));
        const auto error = integer(field(text, "error"));
        c.pll_enabled = u32(field(text, "pll"));
        c.bridge_alive = u32(field(text, "bridge"));
        c.ack_age_ms = u32(field(text, "ack_age_ms"));
        const auto pa4 = integer(field(text, "pa4"));
        const auto dac = integer(field(text, "dac"));
        const char *states[] = {"off", "cal", "acq", "track", "hold"};
        const auto state = field(text, "state");
        c.state = OCH_VCXO_STATE_UNKNOWN;
        for (unsigned i = 0; i < 5; ++i)
            if (state == states[i]) c.state = i;
        const auto k = integer(field(text, "k_mppb")), err = integer(field(text, "err_mppb")),
                   phase = integer(field(text, "phase_ns"));
        c.samples = u32(field(text, "samples"));
        const auto locked = integer(field(text, "locked"));
        c.request_id = u32(field(text, "req"));
        require(c.requested_mode <= 3 && c.applied_mode <= 3 && c.pending <= 1 && c.pll_enabled <= 1 &&
                        c.bridge_alive <= 1 && c.pll_enabled == (c.applied_mode & 1U) &&
                        (c.applied_mode >> 1) == (c.requested_mode >> 1),
                OCH_PROTOCOL, "invalid VCXO mode flags");
        require(error <= 0 && error >= INT32_MIN, OCH_PROTOCOL, "invalid VCXO error code");
        require(c.bridge_alive ? (pa4 == 0 || pa4 == 1) && dac >= 0 && dac <= 4095 && c.state != OCH_VCXO_STATE_UNKNOWN
                               : pa4 == -1 && dac == -1 && c.state == OCH_VCXO_STATE_UNKNOWN && !k && !err && !phase &&
                                         !c.samples,
                OCH_PROTOCOL, "CH32 VCXO fields contradict bridge presence");
        require(c.requested_mode == OCH_VCXO_PPS ? (locked == 0 || locked == 1) : locked == -1, OCH_PROTOCOL,
                "invalid VCXO lock flag");
        require(k >= INT32_MIN && k <= INT32_MAX && err >= INT32_MIN && err <= INT32_MAX && phase >= INT32_MIN &&
                        phase <= INT32_MAX,
                OCH_PROTOCOL, "CH32 VCXO measurement out of range");
        c.error = static_cast<int32_t>(error);
        c.pa4_driving = static_cast<int32_t>(pa4);
        c.dac_code = static_cast<int32_t>(dac);
        c.locked = static_cast<int32_t>(locked);
        c.k_ppb_per_lsb = static_cast<double>(k) / 1000.0;
        c.error_ppb = static_cast<double>(err) / 1000.0;
        c.phase_ns = static_cast<double>(phase);
        return c;
    }
    // The SDK finishes an external-reference request in its main loop, after the
    // CH32 confirms PA4 high-Z (3 s firmware timeout). Poll until it is settled.
    och_vcxo_status settle_vcxo(Device &d) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const auto s = read_vcxo_status(d);
            if (!s.pending) {
                require(!s.error, OCH_PROTOCOL,
                        "SDK VCXO mode change failed (error " + std::to_string(s.error) +
                                "); the CH32 bridge firmware must acknowledge PA4 high-Z before the ADF4002 charge pump is enabled");
                return s;
            }
            require(std::chrono::steady_clock::now() < deadline, OCH_TIMEOUT,
                    "SDK VCXO mode change still pending after 5 s");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}
bool Device::supports_capability(const char *key) {
    if (capabilities.empty()) capabilities = command("system_info?");
    bool supported = false;
    try {
        supported = field(capabilities, key) == "1";
    } catch (const Failure &) {}
    return supported;
}
void Device::require_capability(const char *key) {
    require(supports_capability(key), OCH_UNSUPPORTED,
            std::string("SDK capability ") + key + "=1 required; load the updated firmware/AD9361_TEST.elf");
}
och_pps_status Device::get_pps_status() {
    require_capability("pps_monitor");
    return read_pps_status(*this);
}
void Device::set_pps_enabled(unsigned enabled) {
    require(enabled <= 1, OCH_INVALID, "PPS enabled must be 0 or 1");
    const auto before = get_pps_status();
    if (before.enabled == enabled) return;
    // Only interrupt masks and monitoring state change. Never stop IQ streams
    // or invalidate RF configuration, even if monitoring itself fails.
    command("pps_input_en=" + std::to_string(enabled));
    require(read_pps_status(*this).enabled == enabled, OCH_PROTOCOL, "PPS enable readback mismatch");
    // With the internal reference this starts/stops the CH32 PA4 loop, which
    // the SDK hands over without touching streams or the RF configuration.
    if (supports_capability("vcxo")) (void) settle_vcxo(*this);
}
och_vcxo_status Device::get_vcxo_status() {
    require_capability("vcxo");
    return read_vcxo_status(*this);
}
och_rx_rssi Device::get_rx_rssi() {
    och_rx_rssi c{};
    c.struct_size = sizeof c;
    if (supports_capability("rssi")) {
        const auto text = command("rx_rssi?");
        // The first rssi=1 test build had no switch and always reported.
        c.enabled = text.find(" enabled=") == std::string::npos ? 1 : u32(field(text, "enabled"));
        c.usb_rx_mask = u32(field(text, "rx_channel"));
        for (unsigned i = 0; i < 2; ++i) {
            const auto name = "rx" + std::to_string(i + 1);
            c.valid[i] = u32(field(text, name + "_valid"));
            c.rssi_db[i] = real(field(text, name + "_db"));
        }
    } else {
        // Older SDKs report the same symbol RSSI only inside system_status?.
        const auto text = command("system_status?");
        c.enabled = 1;
        c.usb_rx_mask = u32(field(text, "rx_channel"));
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line))
            if (line.rfind("rssi ", 0) == 0) {
                const auto rx = u32(field(line, "rx"));
                require(rx == 1 || rx == 2, OCH_PROTOCOL, "SDK RSSI channel out of range");
                c.valid[rx - 1] = u32(field(line, "valid"));
                c.rssi_db[rx - 1] = real(field(line, "value"));
            }
    }
    require(c.enabled <= 1 && c.usb_rx_mask <= 3, OCH_PROTOCOL, "SDK RSSI state out of range");
    for (unsigned i = 0; i < 2; ++i) {
        require(c.valid[i] <= 1 && (c.enabled || !c.valid[i]), OCH_PROTOCOL, "SDK RSSI valid flag is inconsistent");
        if (!c.valid[i]) c.rssi_db[i] = 0;
        require(c.rssi_db[i] >= -130 && c.rssi_db[i] <= 0, OCH_PROTOCOL, "SDK RSSI out of range");
    }
    return c;
}
void Device::set_rx_rssi_enabled(unsigned enabled) {
    require(enabled <= 1, OCH_INVALID, "RSSI enabled must be 0 or 1");
    require_capability("rssi");
    const auto reply = command("rssi_enable=" + std::to_string(enabled));
    require(u32(field(reply, "rssi_enable")) == enabled, OCH_PROTOCOL, "SDK RSSI enable readback mismatch");
}
och_lo_source_config Device::get_lo_source() {
    require_capability("external_lo");
    const auto c = read_lo_source(*this);
    lo_sources = int(c.rx_source) | int(c.tx_source) << 1;
    return c;
}
void Device::set_lo_source(bool receive, unsigned source) {
    require(source <= 1, OCH_INVALID, "LO source must be 0 (internal) or 1 (external)");
    const auto before = get_lo_source();
    if ((receive ? before.rx_source : before.tx_source) == source) return;
    lo_sources = -1;
    require(!source || (receive ? before.rx_frequency : before.tx_frequency) <= 4000000000ULL, OCH_INVALID,
            "external LO supports nominal RF frequencies through 4 GHz; set the internal frequency first");
    reconfigure([&] {
        const bool was = configured;
        configured = false;
        transport_dirty = true;
        command(std::string(receive ? "rx_lo_source=" : "tx_lo_source=") + std::to_string(source), 15000);
        const auto after = read_lo_source(*this);
        lo_sources = int(after.rx_source) | int(after.tx_source) << 1;
        require((receive ? after.rx_source : after.tx_source) == source &&
                        (receive ? after.tx_source : after.rx_source) ==
                                (receive ? before.tx_source : before.rx_source) &&
                        (receive ? after.tx_frequency : after.rx_frequency) ==
                                (receive ? before.tx_frequency : before.rx_frequency),
                OCH_PROTOCOL, "LO source readback mismatch; confirm device state before streaming");
        if (was && receive && !config.rx_agc && after.rx_frequency != before.rx_frequency) {
            // Returning to the saved internal frequency can reload another
            // gain table and clamp a manual gain to that band's valid range.
            for (unsigned channel = 0; channel < 2; ++channel) {
                const auto name = "rx" + std::to_string(channel + 1) + "_rf_gain";
                const auto gain = integer(field(command(name + "?"), name));
                require(gain >= -3 && gain <= 76, OCH_PROTOCOL,
                        "manual RX gain readback is out of range after LO source change");
                config.rx_gain[channel] = static_cast<double>(gain);
            }
        }
        config.rx_frequency = after.rx_frequency;
        config.tx_frequency = after.tx_frequency;
        configured = was;
    });
}
och_reference_status Device::get_reference_status() {
    require_capability("adf4002");
    auto status = read_reference_status(*this);
    status.lock_known = 0;
    status.locked = -1;
    // Monitoring is optional. Its absence/failure must not make a healthy
    // reference selection fail. Explicit get_pps_status still reports errors.
    try {
        // PPS without 10 MHz: the CH32 PA4 phase loop is the lock source.
        // 10 MHz only stays unreadable (D5); 10 MHz + PPS keeps the +/-1 ppm check.
        if (supports_capability("vcxo")) {
            const auto v = read_vcxo_status(*this);
            if (v.requested_mode == OCH_VCXO_PPS) {
                status.lock_known = v.bridge_alive;
                status.locked = v.bridge_alive ? v.locked : -1;
                return status;
            }
        }
        const auto pps = get_pps_status();
        status.lock_known = pps.lock_known;
        status.locked = pps.locked;
    } catch (const Failure &) {}
    return status;
}
void Device::set_reference_source(unsigned source) {
    require(source <= 1, OCH_INVALID, "reference source must be 0 (internal) or 1 (external 10 MHz)");
    require_capability("ref_source");
    const auto before = read_reference_status(*this);
    if (reference_healthy(before) && before.source == source) return;
    reconfigure([&] {
        const bool was = configured;
        configured = false;
        transport_dirty = true;
        command("clock_ref_source=" + std::to_string(source), 15000);
        if (supports_capability("vcxo")) (void) settle_vcxo(*this);
        const auto after = read_reference_status(*this);
        require(reference_healthy(after) && after.source == source && after.led_mode == source, OCH_PROTOCOL,
                "reference source readback is unhealthy or mismatched");
        configured = was;
    });
}
och_reference_test_result Device::test_reference() {
    require_capability("adf4002");
    const auto before = read_reference_status(*this);
    require(reference_healthy(before), OCH_INVALID,
            "reference source is unknown or failed; explicitly set reference source to internal before self-test");
    och_reference_test_result result{};
    result.struct_size = sizeof result;
    reconfigure([&] {
        const bool was = configured;
        configured = false;
        transport_dirty = true;
        const auto reply = command("adf4002_self_test!", 15000);
        result.gpio_verified = u32(field(reply, "gpio_verified"));
        result.chip_verified = u32(field(reply, "chip_verified"));
        result.visual_confirmation_required = u32(field(reply, "visual_confirmation_required"));
        require(result.gpio_verified == 1 && result.chip_verified == 0 && result.visual_confirmation_required == 1,
                OCH_PROTOCOL, "ADF4002 self-test requires GPIO verification and human LED confirmation");
        const auto after = read_reference_status(*this);
        require(reference_healthy(after) && after.source == before.source && after.led_mode == before.source,
                OCH_PROTOCOL, "ADF4002 self-test did not restore the previous reference source");
        configured = was;
    });
    return result;
}
och_rx_correction_config Device::get_rx_correction() {
    require_capability("rx_correction");
    return read_rx_correction(*this);
}
void Device::set_rx_correction(bool dc, unsigned enabled) {
    require(enabled <= 1, OCH_INVALID, "RX correction enabled must be 0 or 1");
    // Preflight only reads control state: unsupported firmware and a no-op
    // leave active streams, their queues and the configuration cache intact.
    const auto before = get_rx_correction();
    if ((dc ? before.dc_enabled : before.iq_enabled) == enabled) return;
    auto requested = before;
    (dc ? requested.dc_enabled : requested.iq_enabled) = enabled;
    reconfigure([&] {
        const bool was = configured;
        configured = false;
        transport_dirty = true;
        command(std::string(dc ? "rx_dc_correction=" : "rx_iq_correction=") + std::to_string(enabled), 15000);
        const auto after = read_rx_correction(*this);
        require(after.dc_enabled == requested.dc_enabled && after.iq_enabled == requested.iq_enabled, OCH_PROTOCOL,
                "RX correction readback mismatch; confirm device state before streaming");
        configured = was;
    });
}
och_clock_config Device::get_clock() {
    auto text = command("clock_chain?");
    och_clock_config c{};
    c.struct_size = sizeof c;
    c.reference_hz = u32(field(text, "ref"));
    clocks(field(text, "rx"), c.rx_hz);
    clocks(field(text, "tx"), c.tx_hz);
    return c;
}
och_fir_config Device::get_fir(unsigned direction) {
    require(direction <= OCH_TX, OCH_INVALID, "invalid FIR direction");
    require_capability("fir_upload");
    std::string prefix = direction == OCH_RX ? "rx" : "tx";
    auto text = command(prefix + "_fir_config?");
    och_fir_config c{};
    c.struct_size = sizeof c;
    c.direction = direction;
    c.channel_mask = 3;
    c.factor = u32(field(text, "factor"));
    c.tap_count = u32(field(text, "taps"));
    auto gain = integer(field(text, "gain_dB"));
    require(gain >= -12 && gain <= 6, OCH_PROTOCOL, "SDK FIR gain out of range");
    c.gain_db = static_cast<int32_t>(gain);
    c.enabled = u32(field(text, "enabled"));
    c.verified = u32(field(text, "verified"));
    c.crc32 = u32(field(text, "crc32"));
    valid(c);
    require(c.verified <= 1, OCH_PROTOCOL, "SDK FIR verified flag must be 0 or 1");
    for (unsigned offset = 0; offset < c.tap_count; offset += 8) {
        auto n = std::min(8U, c.tap_count - offset);
        auto line = command(prefix + "_fir_read=" + std::to_string(offset) + "," + std::to_string(n));
        auto pos = line.find(prefix + "_fir_read=");
        require(pos != std::string::npos, OCH_PROTOCOL, "missing FIR coefficient reply");
        std::istringstream in(line.substr(pos + prefix.size() + 10)); // prefix + _fir_read=
        int64_t actual = -1, value = 0;
        require(bool(in >> actual) && actual == offset, OCH_PROTOCOL, "FIR read offset mismatch");
        for (unsigned i = 0; i < n; ++i) {
            require(bool(in >> value) && value >= -32768 && value <= 32767, OCH_PROTOCOL,
                    "FIR coefficient read out of range");
            c.coefficients[offset + i] = static_cast<int16_t>(value);
        }
        std::string extra;
        require(!(in >> extra), OCH_PROTOCOL, "extra FIR coefficient reply data");
    }
    require(crc(c.coefficients, c.tap_count) == c.crc32, OCH_PROTOCOL, "FIR readback CRC mismatch");
    return c;
}
void Device::set_fir(const och_fir_config &c) {
    valid(c);
    if (!reconfiguring_control) require_capability("fir_upload");
    if ((rx || tx) && !reconfiguring_control) {
        reconfigure([&] { set_fir(c); });
        return;
    }
    auto before = get_clock();
    bool was = configured;
    configured = false;
    transport_dirty = true;
    const auto id = token();
    std::string sid = std::to_string(id), prefix = c.direction == OCH_RX ? "rx" : "tx";
    try {
        command(prefix + "_fir_begin=" + sid + "," + std::to_string(c.tap_count) + "," + std::to_string(c.factor) +
                "," + std::to_string(c.gain_db));
        for (unsigned offset = 0; offset < c.tap_count; offset += 8) {
            std::string line = "fir_data=" + sid + "," + std::to_string(offset);
            for (unsigned i = offset; i < std::min(offset + 8, c.tap_count); ++i)
                line += "," + std::to_string(c.coefficients[i]);
            command(line);
        }
        command("fir_commit=" + sid + "," + std::to_string(c.enabled) + "," +
                        std::to_string(crc(c.coefficients, c.tap_count)),
                15000);
        auto actual = get_fir(c.direction);
        require(actual.verified == 1 && actual.factor == c.factor && actual.gain_db == c.gain_db &&
                        actual.tap_count == c.tap_count && actual.enabled == c.enabled &&
                        std::equal(c.coefficients, c.coefficients + c.tap_count, actual.coefficients),
                OCH_PROTOCOL, "FIR commit readback mismatch");
        unchanged_rate(before, get_clock());
        configured = was;
    } catch (...) {
        auto error = std::current_exception();
        try {
            command("fir_abort=" + sid);
        } catch (...) {}
        std::rethrow_exception(error);
    }
}
void Device::enable_fir(unsigned r, unsigned t) {
    require(r <= 1 && t <= 1, OCH_INVALID, "FIR states must be 0 or 1");
    if (!reconfiguring_control) require_capability("fir_upload");
    if ((rx || tx) && !reconfiguring_control) {
        reconfigure([&] { enable_fir(r, t); });
        return;
    }
    auto before = get_clock();
    bool was = configured;
    configured = false;
    transport_dirty = true;
    command("fir_enable=" + std::to_string(r) + "," + std::to_string(t), 15000);
    auto rr = get_fir(OCH_RX), tt = get_fir(OCH_TX);
    require(rr.enabled == r && tt.enabled == t && rr.verified && tt.verified, OCH_PROTOCOL,
            "FIR enable readback mismatch");
    unchanged_rate(before, get_clock());
    configured = was;
}
void Device::set_clock(const och_clock_config &c) {
    require(c.struct_size == sizeof c, OCH_INVALID, "clock config ABI mismatch");
    for (unsigned i = 0; i < 6; ++i) require(c.rx_hz[i] && c.tx_hz[i], OCH_INVALID, "clock rates must be positive");
    require(c.rx_hz[5] == c.tx_hz[5] && c.rx_hz[5] <= 61440000 && c.rx_hz[0] == c.tx_hz[0], OCH_INVALID,
            "shared RX/TX sample rate and BBPLL required");
    if (!reconfiguring_control) {
        require_capability("clock_config");
        const auto before = get_clock();
        require(!c.reference_hz || c.reference_hz == before.reference_hz, OCH_UNSUPPORTED,
                "reference clock is fixed by this board");
        if (rx || tx) {
            reconfigure([&] { set_clock(c); });
            return;
        }
    }
    bool was = configured;
    configured = false;
    transport_dirty = true;
    auto id = std::to_string(token());
    try {
        command("clock_begin=" + id);
        command("clock_rx=" + id + "," + csv(c.rx_hz));
        command("clock_tx=" + id + "," + csv(c.tx_hz));
        command("clock_commit=" + id, 15000);
        auto after = get_clock();
        require(std::equal(c.rx_hz, c.rx_hz + 6, after.rx_hz) && std::equal(c.tx_hz, c.tx_hz + 6, after.tx_hz),
                OCH_PROTOCOL, "clock chain readback mismatch");
        config.sample_rate = requested_sample_rate = after.rx_hz[5];
        configured = was;
    } catch (...) {
        auto error = std::current_exception();
        try {
            command("clock_abort=" + id);
        } catch (...) {}
        std::rethrow_exception(error);
    }
}
void Device::set_sample_rate(uint32_t rate) {
    require(rate > 0 && rate <= 61440000, OCH_INVALID, "sample rate must be 1..61440000 Hz");
    if (configured && (config.sample_rate == rate || requested_sample_rate == rate)) return;
    if (!reconfiguring_control) require_capability("clock_config");
    if ((rx || tx) && !reconfiguring_control) {
        reconfigure([&] { set_sample_rate(rate); });
        return;
    }
    bool was = configured;
    configured = false;
    transport_dirty = true;
    config.sample_rate = apply_sample_rate(rate);
    configured = was;
}
// The SDK rejects a rate whose fractional-N BBPLL cannot be programmed
// exactly, e.g. 11.2 MSPS needs 716.8 MHz. Plan on the host instead of
// probing: mirror the driver's clock-chain choice for the current FIR
// factors and take the nearest whole-Hz rate (+d before -d) whose BBPLL
// round-trips, typically ~11 Hz away. One SDK attempt then suffices. If the
// SDK still rejects it (model out of step with firmware), search the device
// 1 Hz at a time as before.
uint32_t Device::apply_sample_rate(uint32_t rate) {
    constexpr uint32_t max_rate = 61440000, max_plan_offset = 4096, max_probe_offset = 64;
    const auto attempt = [&](uint32_t hz) {
        command("clock_sample_rate=" + std::to_string(hz), 15000);
        auto actual = get_clock();
        require(actual.rx_hz[5] == hz && actual.tx_hz[5] == hz, OCH_PROTOCOL, "sample rate readback mismatch");
    };
    const auto clock_rejected = [](const Failure &f) {
        return f.code == OCH_PROTOCOL && std::string(f.what()).find("fir_or_clock_apply") != std::string::npos;
    };
    // The current chain carries the FIR factors: RF/Fs on each side.
    const auto now = get_clock();
    const uint32_t rx_intdec = now.rx_hz[5] ? now.rx_hz[4] / now.rx_hz[5] : 1,
                   tx_intdec = now.tx_hz[5] ? now.tx_hz[4] / now.tx_hz[5] : 1;
    const auto plannable = [&](uint64_t hz) {
        if (hz < 1 || hz > max_rate) return false; // rate-d below 0 wraps above max_rate
        const auto bbpll = planned_bbpll(static_cast<uint32_t>(hz), rx_intdec, tx_intdec);
        return bbpll && bbpll_exact(bbpll, now.reference_hz);
    };
    uint32_t target = rate;
    if (planned_bbpll(rate, rx_intdec, tx_intdec) && !plannable(rate)) {
        for (uint32_t d = 1; d <= max_plan_offset && target == rate; ++d)
            for (const uint64_t hz: {uint64_t(rate) + d, uint64_t(rate) - d})
                if (plannable(hz)) {
                    target = static_cast<uint32_t>(hz);
                    break;
                }
    }
    try {
        attempt(target);
        requested_sample_rate = rate;
        return target;
    } catch (const Failure &original) {
        if (!clock_rejected(original)) throw;
        for (uint32_t d = 1; d <= max_probe_offset; ++d) {
            for (const uint64_t hz: {uint64_t(rate) + d, uint64_t(rate) - d}) {
                if (hz < 1 || hz > max_rate || hz == target) continue;
                try {
                    attempt(static_cast<uint32_t>(hz));
                    requested_sample_rate = rate;
                    return static_cast<uint32_t>(hz);
                } catch (const Failure &f) {
                    if (!clock_rejected(f)) throw;
                }
            }
        }
        throw;
    }
}
