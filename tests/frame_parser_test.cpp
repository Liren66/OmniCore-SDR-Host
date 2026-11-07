// Offline test of the framed-transport parser/writer (no device needed).
// g++ -std=c++17 -O2 -I src tests/frame_parser_test.cpp src/dsp/frame.cpp
#include "dsp/frame.hpp"
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace och::dsp;

struct Seen { FrameHeader h; std::vector<uint8_t> payload; };
static std::vector<Seen> seen;
static void sink(void*, const FrameHeader& h, const uint8_t* p, size_t n) {
    seen.push_back({h, std::vector<uint8_t>(p, p + n)});
}

static std::vector<uint8_t> make_block(uint16_t seq, uint16_t valid, uint64_t t, uint8_t flags, unsigned frame) {
    std::vector<uint8_t> b(frame_block_bytes, 0);
    FrameHeader h; h.flags = flags; h.seq = seq; h.valid = valid; h.time = t;
    write_frame_header(b.data(), h);
    for (size_t i = 0; i < size_t(valid) * frame; ++i) b[frame_header_bytes + i] = uint8_t(t + i);
    return b;
}

int main() {
    int errors = 0;
    const unsigned frame = 6;
    std::vector<uint8_t> stream;
    // 300 bytes of stale garbage, then 6 blocks, a corrupted block, 3 more blocks.
    std::mt19937 rng(1);
    for (int i = 0; i < 300; ++i) stream.push_back(uint8_t(rng()));
    uint64_t t = 0x1'0000'5000ULL;
    for (uint16_t s = 0; s < 6; ++s) {
        auto b = make_block(s, s == 5 ? 100 : 2728, t, s == 0 ? 3 : 1, frame);
        stream.insert(stream.end(), b.begin(), b.end());
        t += 2728;
    }
    auto bad = make_block(6, 2728, t, 1, frame);
    bad[0] ^= 0x55;                                   // check byte corrupted
    stream.insert(stream.end(), bad.begin(), bad.end());
    t += 2728;
    for (uint16_t s = 7; s < 10; ++s) {
        auto b = make_block(s, 2728, t, s == 7 ? 5 : 1, frame);   // GAP flag after the bad block
        stream.insert(stream.end(), b.begin(), b.end());
        t += 2728;
    }
    // Feed in irregular chunks to exercise straddling headers/blocks.
    FrameParser parser(frame);
    size_t pos = 0;
    while (pos < stream.size()) {
        size_t n = std::min<size_t>(stream.size() - pos, 1 + rng() % 20000);
        parser.feed(stream.data() + pos, n, sink, nullptr);
        pos += n;
    }
    if (seen.size() != 9) { std::printf("expected 9 blocks, got %zu\n", seen.size()); ++errors; }
    for (size_t i = 0; i < seen.size(); ++i) {
        const auto& s = seen[i];
        const uint16_t want_seq = uint16_t(i < 6 ? i : i + 1);
        if (s.h.seq != want_seq) { std::printf("block %zu seq %u\n", i, s.h.seq); ++errors; }
        if (s.payload.size() != size_t(s.h.valid) * frame) ++errors;
        for (size_t k = 0; k < s.payload.size(); ++k)
            if (s.payload[k] != uint8_t(s.h.time + k)) { ++errors; break; }
    }
    if (parser.invalid() != 1) { std::printf("invalid=%llu\n", (unsigned long long)parser.invalid()); ++errors; }
    // Header round trip of extreme values.
    uint8_t h16[16];
    FrameHeader a; a.flags = 0xFF; a.seq = 0xFFFF; a.valid = 5456; a.time = ~0ULL;
    write_frame_header(h16, a);
    FrameHeader b2;
    if (!parse_frame_header(h16, b2) || b2.time != a.time || b2.seq != a.seq || b2.valid != a.valid || b2.flags != a.flags) ++errors;
    h16[9] ^= 1;
    if (parse_frame_header(h16, b2)) ++errors;
    std::printf("blocks=%llu invalid=%llu resyncs=%llu\n", (unsigned long long)parser.blocks(),
                (unsigned long long)parser.invalid(), (unsigned long long)parser.resyncs());
    std::printf("FRAME PARSER TEST: %s\n", errors ? "FAIL" : "PASS");
    return errors ? 1 : 0;
}
