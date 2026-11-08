#include "internal.hpp"
#include <iomanip>
#include <map>
#include <sstream>
using namespace och::core;

namespace {
    std::string fixed(double n, int precision = 6) {
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s << std::fixed << std::setprecision(precision) << n;
        return s.str();
    }
    void validate(const och_config &c) {
        require(c.struct_size == sizeof c, OCH_INVALID, "och_config ABI size mismatch");
        require(c.usb_iq_bits == 0 || c.usb_iq_bits == 12 || c.usb_iq_bits == 8, OCH_INVALID,
                "usb_iq_bits must be 0 (default), 12 or 8");
        require(iq_bits(c) != 8 || (c.rx_mask == 3 && c.tx_mask == 3), OCH_INVALID,
                "8-bit USB IQ requires rx_mask=tx_mask=3");
        require(c.sample_rate > 0 && c.sample_rate <= 61440000 && c.rx_mask <= 3 && c.tx_mask <= 3 &&
                        (c.rx_mask || c.tx_mask),
                OCH_INVALID, "invalid rate/channel masks");
        require(c.rx_frequency >= 70000000 && c.rx_frequency <= 6000000000ULL && c.tx_frequency >= 70000000 &&
                        c.tx_frequency <= 6000000000ULL,
                OCH_INVALID, "RF frequency must be 70 MHz..6 GHz");
        require(c.rx_agc <= 3, OCH_INVALID, "invalid RX gain mode");
        require(c.rx_bandwidth >= 200000 && c.rx_bandwidth <= 56000000 && c.tx_bandwidth >= 200000 &&
                        c.tx_bandwidth <= 56000000,
                OCH_INVALID, "RF bandwidth must be 200 kHz..56 MHz");
        for (unsigned i = 0; i < 2; ++i) {
            require(std::isfinite(c.rx_gain[i]) && c.rx_gain[i] >= -3 && c.rx_gain[i] <= 76 &&
                            c.rx_gain[i] == std::floor(c.rx_gain[i]),
                    OCH_INVALID, "RX manual gain must be integer dB in -3..76, device band may further restrict");
            require(std::isfinite(c.tx_attenuation[i]) && c.tx_attenuation[i] >= 0 && c.tx_attenuation[i] <= 89.75 &&
                            c.tx_attenuation[i] * 4 == std::floor(c.tx_attenuation[i] * 4),
                    OCH_INVALID, "TX attenuation must be 0..89.75 dB in 0.25 dB steps");
        }
    }
    bool same(const och_config &a, const och_config &b) {
        return iq_bits(a) == iq_bits(b) && a.sample_rate == b.sample_rate && a.rx_mask == b.rx_mask &&
               a.tx_mask == b.tx_mask && a.rx_frequency == b.rx_frequency && a.tx_frequency == b.tx_frequency &&
               a.rx_bandwidth == b.rx_bandwidth && a.tx_bandwidth == b.tx_bandwidth && a.rx_agc == b.rx_agc &&
               a.rx_gain[0] == b.rx_gain[0] && a.rx_gain[1] == b.rx_gain[1] &&
               a.tx_attenuation[0] == b.tx_attenuation[0] && a.tx_attenuation[1] == b.tx_attenuation[1];
    }
    // Fields retune() may change without stopping IQ streams.
    bool tuning_only(const och_config &a, const och_config &b) {
        och_config t = a;
        t.rx_frequency = b.rx_frequency;
        t.tx_frequency = b.tx_frequency;
        for (unsigned i = 0; i < 2; ++i) {
            t.rx_gain[i] = b.rx_gain[i];
            t.tx_attenuation[i] = b.tx_attenuation[i];
        }
        return same(t, b);
    }
    // SDK RX gain tables cover (0,1.3], (1.3,4] and (4,6] GHz. A retune near an
    // edge is treated as a table change because the PLL rounds the LO.
    unsigned gain_band(uint64_t hz) { return hz <= 1300000000ULL ? 0 : hz <= 4000000000ULL ? 1 : 2; }
    bool near_band_edge(uint64_t hz) {
        for (uint64_t edge: {1300000000ULL, 4000000000ULL})
            if ((hz > edge ? hz - edge : edge - hz) < 1000000) return true;
        return false;
    }
    void set_iq_bits(och_device &d, unsigned bits) {
        d.command("iq_format=" + std::to_string(bits));
        const auto reply = d.command("iq_format?");
        std::map<std::string, std::string> fields;
        std::istringstream input(reply);
        std::string token;
        while (input >> token) {
            const auto at = token.find('=');
            if (at == std::string::npos) continue;
            require(fields.emplace(token.substr(0, at), token.substr(at + 1)).second, OCH_PROTOCOL,
                    "duplicate IQ format reply field");
        }
        const auto expected = std::to_string(bits);
        require(fields["iq_format"] == expected && fields["requested"] == expected && fields["applied"] == expected &&
                        fields["cfg_busy"] == "0" && fields["cfg_error"] == "0" && fields["verified"] == "1" &&
                        fields["scope"] == "2tx2rx" &&
                        (fields["supported"] == "12,8" || (bits == 12 && fields["supported"] == "12")),
                OCH_PROTOCOL, "IQ wire format readback mismatch; configure again before streaming");
    }
}

void och_device::configure(const och_config &requested) {
    validate(requested);
    // A rate already mapped to its nearest accepted clock plan is unchanged.
    och_config c = requested;
    if (configured && c.sample_rate == requested_sample_rate) c.sample_rate = config.sample_rate;
    // Existing buffers have a fixed sample layout. Even equal-width input
    // switches are explicit stream recreation, not a hidden layout change.
    require(!(rx || tx) ||
                    (c.rx_mask == config.rx_mask && c.tx_mask == config.tx_mask && iq_bits(c) == iq_bits(config)),
            OCH_BUSY, "destroy streams before changing channel masks or USB IQ format");
    if (configured && same(c, config)) return;
    if (iq_bits(c) == 8) {
        require_capability("iq_format");
        require(supports_capability("iq8_2t2r"), OCH_UNSUPPORTED,
                "8-bit USB IQ requires iq8_2t2r=1: load the matching IQ8 FPGA bitstream (CH32H417_IP 1.1) and updated SDK");
    }
    // A source is selected independently of this ABI-1 structure. Validate
    // external nominal RF limits before interrupting any existing stream.
    if ((!configured || c.rx_frequency != config.rx_frequency || c.tx_frequency != config.tx_frequency) &&
        supports_capability("external_lo")) {
        if (lo_sources < 0) get_lo_source();
        require(!(lo_sources & 1) || c.rx_frequency <= 4000000000ULL, OCH_INVALID,
                "external RX LO nominal RF frequency must be 70 MHz..4 GHz");
        require(!(lo_sources & 2) || c.tx_frequency <= 4000000000ULL, OCH_INVALID,
                "external TX LO nominal RF frequency must be 70 MHz..4 GHz");
    }
    // LO/gain/attenuation changes keep streams running. A TX LO change with
    // a TX stream still stops it: a large step runs TX quadrature calibration.
    if (configured && (rx || tx) && (!rx || !rx->lease) && tuning_only(c, config) &&
        (!tx || c.tx_frequency == config.tx_frequency)) {
        retune(c);
        return;
    }
    reconfigure([&] { apply_config(c); });
}

void och_device::retune(const och_config &c) {
    const auto before = config;
    const auto began = now_ns();
    configured = false;
    try {
        const bool table = tune_lo(c, before, false);
        tune_gain(c, before, false, table);
    } catch (...) {
        // Streams kept running with partly applied settings. Stop them like
        // any failed reconfiguration; this rethrows the original failure.
        const auto failure = std::current_exception();
        reconfigure([&] { std::rethrow_exception(failure); });
        throw;
    }
    config = c;
    config.sample_rate = before.sample_rate;
    configured = true;
    if (rx) rx->retuned = true;
    emit(OCH_EVENT_RECONFIGURED,
         "retune complete streams_continued=1 rx_boundary=approximate elapsed_us=" +
                 std::to_string((now_ns() - began) / 1000),
         rx ? rx->epoch_base : 0);
}

bool och_device::tune_lo(const och_config &c, const och_config &before, bool full) {
    const bool rx_tuned = full || c.rx_frequency != before.rx_frequency;
    if (rx_tuned) command("rx_lo_freq=" + fixed(c.rx_frequency / 1e6));
    if (full || c.tx_frequency != before.tx_frequency) command("tx_lo_freq=" + fixed(c.tx_frequency / 1e6));
    return full || (rx_tuned && (gain_band(c.rx_frequency) != gain_band(before.rx_frequency) ||
                                 near_band_edge(c.rx_frequency) || near_band_edge(before.rx_frequency)));
}

void och_device::tune_gain(const och_config &c, const och_config &before, bool full, bool gain_table_changed) {
    for (unsigned i = 0; i < 2; ++i) {
        const auto ch = std::to_string(i + 1);
        if (full || c.rx_agc != before.rx_agc) command("rx" + ch + "_gc_mode=" + std::to_string(c.rx_agc));
        // A new gain table can clamp or remap manual gain, so reapply it.
        if (!c.rx_agc && (gain_table_changed || c.rx_agc != before.rx_agc || c.rx_gain[i] != before.rx_gain[i]))
            command("rx" + ch + "_rf_gain=" + fixed(c.rx_gain[i], 0));
        if (full || c.tx_attenuation[i] != before.tx_attenuation[i])
            command("tx" + ch + "_attenuation=" + fixed(c.tx_attenuation[i] * 1000, 0));
    }
}

void och_device::apply_config(const och_config &c) {
    const auto before = config;
    const bool full = !configured;
    configured = false;
    try {
        if (!reconfiguring_control) {
            command("usb_stop!");
            rx_on = tx_on = false;
            transport_dirty = true;
            reset_transport();
        }
        const bool wire_change = full || iq_bits(c) != iq_bits(before);
        const bool wire_control = wire_change && supports_capability("iq_format");
        // An earlier handle can leave IQ8 selected. Restore the legacy layout
        // before any mask/source changes; old SDKs receive no new command.
        if (wire_control) set_iq_bits(*this, 12);
        if (full || c.rx_mask != before.rx_mask) command("rx_channel=" + std::to_string(c.rx_mask));
        if (full || c.tx_mask != before.tx_mask) command("tx_channel=" + std::to_string(c.tx_mask));
        if (full) command("tx_source=1");
        if (wire_change && iq_bits(c) == 8) {
            require(wire_control, OCH_UNSUPPORTED, "SDK iq_format=1 is required");
            set_iq_bits(*this, 8);
        }
        uint32_t applied_rate = c.sample_rate;
        if (full || c.sample_rate != before.sample_rate) {
            if (supports_capability("clock_config"))
                applied_rate = apply_sample_rate(c.sample_rate);
            else {
                command("rx_samp_freq=" + std::to_string(c.sample_rate), 15000);
                requested_sample_rate = c.sample_rate;
            }
        }
        const bool gain_table_changed = tune_lo(c, before, full);
        if (full || c.rx_bandwidth != before.rx_bandwidth) command("rx_rf_bandwidth=" + std::to_string(c.rx_bandwidth));
        if (full || c.tx_bandwidth != before.tx_bandwidth) command("tx_rf_bandwidth=" + std::to_string(c.tx_bandwidth));
        tune_gain(c, before, full, gain_table_changed);
        if (full || c.sample_rate != before.sample_rate) {
            const auto clocks = get_clock();
            require(clocks.rx_hz[5] == applied_rate && clocks.tx_hz[5] == applied_rate, OCH_PROTOCOL,
                    "requested shared sample rate was not applied");
        }
        if (!reconfiguring_control) command("usb_clear!");
        config = c;
        config.sample_rate = applied_rate;
        configured = true;
    } catch (...) {
        transport_dirty = true;
        if (!reconfiguring_control) try {
                command("usb_stop!");
                rx_on = tx_on = false;
            } catch (...) {}
        throw;
    }
}
