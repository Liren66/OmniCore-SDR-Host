#pragma once
#include <omnicore/och.h>
#include <array>
#include <cstring>

// Host-specific measurements, not device capabilities or enforced limits.
// Evidence and reproduction commands: docs/usb2.md.
namespace och_cli::usb2_reference {
    struct entry {
        const char *id;
        const char *label;
        unsigned rx_channels, tx_channels, bits;
        uint32_t rate_hz, seconds;
    };
    inline constexpr const char *firmware = "SDK_CMD_1.0_20261024_IQ8";
    inline constexpr const char *date = "2026-10-25";
    inline constexpr const char *host = "ThinkPad E14 Gen 7 / Core Ultra 7 255H";
    inline constexpr std::array<entry, 7> entries = {{
            {"rx1", "1 RX", 1, 0, 12, 14000000, 300},
            {"rx2", "2 RX", 2, 0, 12, 7000000, 300},
            {"tx1", "1 TX", 0, 1, 12, 14000000, 300},
            {"tx2", "2 TX", 0, 2, 12, 6500000, 300},
            {"duplex1", "1 RX + 1 TX", 1, 1, 12, 7000000, 300},
            {"duplex2", "2 RX + 2 TX", 2, 2, 12, 3500000, 300},
            {"duplex8", "2 RX + 2 TX", 2, 2, 8, 5000000, 300},
    }};
    inline bool applies(const och_device_info &i) {
        return i.usb_speed_mbps == 480 && std::strcmp(i.firmware, firmware) == 0;
    }
    inline unsigned channels(uint32_t mask) { return (mask & 1u) + ((mask >> 1u) & 1u); }
    inline const entry *find(const och_device_info &i, const och_config &c) {
        if (!applies(i) || c.rx_mask > 3 || c.tx_mask > 3) return nullptr;
        // Single-channel measurements used CH1; CH2-only sessions are unmeasured.
        if (c.rx_mask == 2 || c.tx_mask == 2) return nullptr;
        for (const auto &e: entries)
            if (e.rate_hz && e.rx_channels == channels(c.rx_mask) && e.tx_channels == channels(c.tx_mask) &&
                e.bits == (c.usb_iq_bits ? c.usb_iq_bits : 12u))
                return &e;
        return nullptr;
    }
} // namespace och_cli::usb2_reference
