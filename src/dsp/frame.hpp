#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace och::dsp {

    // Framed USB transport of FPGA CH32H417_IP 1.3 (the only transport).
    // Every 16 KiB UHSIF block = 16-byte header + 16368-byte payload. 16368 is a
    // multiple of 3, 4, 6 and 12, so every block holds whole samples.
    // Header words are little-endian u32:
    //   h0 = 0x4F43'0000 | flags << 8 | check, h1 = seq << 16 | valid,
    //   h2/h3 = sample time (ticks) of the first payload sample.
    // check = XOR of h0 bytes 1..3 and all bytes of h1..h3.
    constexpr std::size_t frame_block_bytes = 16384;
    constexpr std::size_t frame_header_bytes = 16;
    constexpr std::size_t frame_payload_bytes = frame_block_bytes - frame_header_bytes;

    enum : std::uint8_t {
        // RX
        frame_rx_time_valid = 1,
        frame_rx_sob = 2,
        frame_rx_gap = 4,
        frame_rx_eob = 8,
        frame_rx_late = 16,
        frame_rx_pad = 32,
        // TX
        frame_tx_has_time = 1,
        frame_tx_sob = 2,
        frame_tx_eob = 8,
    };

    struct FrameHeader {
        std::uint8_t flags = 0;
        std::uint16_t seq = 0;
        std::uint16_t valid = 0; // valid samples (frames) in this block
        std::uint64_t time = 0;
    };

    bool parse_frame_header(const std::uint8_t *p, FrameHeader &h) noexcept;
    void write_frame_header(std::uint8_t *p, const FrameHeader &h) noexcept;

    // Streaming RX parser. Keeps 16 KiB alignment, re-synchronizes by searching
    // for a valid header after a corrupt one, and hands each block's valid
    // payload bytes (a whole number of wire frames) to the sink.
    class FrameParser {
    public:
        using Sink = void (*)(void *context, const FrameHeader &header, const std::uint8_t *payload, std::size_t bytes);
        explicit FrameParser(std::size_t frame_bytes);
        void reset() noexcept;
        void feed(const std::uint8_t *data, std::size_t size, Sink sink, void *context);
        std::uint64_t blocks() const noexcept { return blocks_; }
        std::uint64_t resyncs() const noexcept { return resyncs_; }
        std::uint64_t invalid() const noexcept { return invalid_; }
        bool synchronized() const noexcept { return synced_; }

    private:
        bool block(const std::uint8_t *b, Sink sink, void *context);
        std::size_t frame_;
        std::array<std::uint8_t, frame_block_bytes> buf_{};
        std::size_t have_ = 0;
        std::array<std::uint8_t, frame_header_bytes> carry_{};
        std::size_t carry_n_ = 0;
        bool synced_ = false;
        std::uint64_t blocks_ = 0, resyncs_ = 0, invalid_ = 0;
    };

} // namespace och::dsp
