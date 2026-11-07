#include "frame.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace och::dsp {
namespace {
std::uint32_t rd32(const std::uint8_t* p) noexcept {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 |
           std::uint32_t(p[3]) << 24;
}
void wr32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8);
    p[2] = std::uint8_t(v >> 16); p[3] = std::uint8_t(v >> 24);
}
std::uint8_t check_of(const std::uint8_t* p) noexcept {
    std::uint8_t c = p[1] ^ p[2] ^ p[3];
    for (std::size_t i = 4; i < frame_header_bytes; ++i) c ^= p[i];
    return c;
}
} // namespace

bool parse_frame_header(const std::uint8_t* p, FrameHeader& h) noexcept {
    if (p[2] != 0x43 || p[3] != 0x4F || p[0] != check_of(p)) return false;
    const auto h1 = rd32(p + 4);
    h.flags = p[1];
    h.seq = std::uint16_t(h1 >> 16);
    h.valid = std::uint16_t(h1);
    h.time = std::uint64_t(rd32(p + 8)) | std::uint64_t(rd32(p + 12)) << 32;
    return true;
}

void write_frame_header(std::uint8_t* p, const FrameHeader& h) noexcept {
    wr32(p + 4, std::uint32_t(h.seq) << 16 | h.valid);
    wr32(p + 8, std::uint32_t(h.time));
    wr32(p + 12, std::uint32_t(h.time >> 32));
    p[1] = h.flags; p[2] = 0x43; p[3] = 0x4F;
    p[0] = check_of(p);
}

FrameParser::FrameParser(std::size_t frame_bytes) : frame_(frame_bytes) {
    if (!frame_bytes || frame_payload_bytes % frame_bytes)
        throw std::invalid_argument("frame size must divide the 16368-byte payload");
}

void FrameParser::reset() noexcept {
    have_ = carry_n_ = 0;
    synced_ = false;
    blocks_ = resyncs_ = invalid_ = 0;
}

bool FrameParser::block(const std::uint8_t* b, Sink sink, void* context) {
    FrameHeader h;
    if (!parse_frame_header(b, h) || std::size_t(h.valid) * frame_ > frame_payload_bytes) {
        ++invalid_;
        return false;
    }
    ++blocks_;
    sink(context, h, b + frame_header_bytes, std::size_t(h.valid) * frame_);
    return true;
}

void FrameParser::feed(const std::uint8_t* p, std::size_t n, Sink sink, void* context) {
    if (!n) return;
    if (!p || !sink) throw std::invalid_argument("frame parser requires data and sink");
    while (n) {
        if (!synced_) {
            // Search carry + data for a complete valid header.
            std::array<std::uint8_t, frame_header_bytes> tmp{};
            const std::size_t c = carry_n_, total = c + n;
            std::size_t found = total;
            for (std::size_t k = 0; k + frame_header_bytes <= total; ++k) {
                const std::uint8_t* q;
                if (k < c) {
                    for (std::size_t i = 0; i < frame_header_bytes; ++i)
                        tmp[i] = (k + i < c) ? carry_[k + i] : p[k + i - c];
                    q = tmp.data();
                } else {
                    q = p + (k - c);
                    if (q[2] != 0x43 || q[3] != 0x4F) continue;
                }
                FrameHeader h;
                if (parse_frame_header(q, h) && std::size_t(h.valid) * frame_ <= frame_payload_bytes) {
                    found = k;
                    break;
                }
            }
            if (found == total) {
                const std::size_t keep = std::min<std::size_t>(frame_header_bytes - 1, total);
                std::array<std::uint8_t, frame_header_bytes> last{};
                for (std::size_t i = 0; i < keep; ++i) {
                    const std::size_t k = total - keep + i;
                    last[i] = k < c ? carry_[k] : p[k - c];
                }
                carry_ = last;
                carry_n_ = keep;
                return;
            }
            if (blocks_ || invalid_) ++resyncs_;
            synced_ = true;
            have_ = 0;
            if (found < c) {
                have_ = c - found;
                std::memcpy(buf_.data(), carry_.data() + found, have_);
            } else {
                p += found - c;
                n -= found - c;
            }
            carry_n_ = 0;
            continue;
        }
        if (!have_ && n >= frame_block_bytes) {
            if (!block(p, sink, context)) {
                synced_ = false;
                ++p; --n;          // search again from the next byte
                continue;
            }
            p += frame_block_bytes;
            n -= frame_block_bytes;
            continue;
        }
        const std::size_t take = std::min(n, frame_block_bytes - have_);
        std::memcpy(buf_.data() + have_, p, take);
        have_ += take; p += take; n -= take;
        if (have_ == frame_block_bytes) {
            have_ = 0;
            if (!block(buf_.data(), sink, context)) synced_ = false;
        }
    }
}

} // namespace och::dsp
