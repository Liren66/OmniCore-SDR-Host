#pragma once

#include <cstddef>
#include <cstdint>

namespace och::dsp {

    enum class Format { packed12, sc16, fc32 };

    // Bytes per complex sample, independent of channel count.
    std::size_t sample_bytes(Format format) noexcept;
    bool avx2_available() noexcept;
    const char *implementation_name() noexcept;

    // Wire: I[11:4], I[3:0]:Q[11:8], Q[7:0], signed two's-complement.
    // sc16 is signed, LEFT aligned: -32768..32752, step 16.
    // fc32 is normalized by 2048: [-1, 1). Packing rounds ties away from
    // zero and saturates. NaN becomes zero; infinities saturate.
    // Samples remain interleaved in wire channel order. Input/output must not
    // overlap. No SIMD alignment or buffer padding is required (typed IQ arrays
    // retain their natural C++ alignment). nullptr is accepted for count == 0.
    void unpack_sc16(const std::uint8_t *wire, std::int16_t *iq, std::size_t complex_samples) noexcept;
    void unpack_fc32(const std::uint8_t *wire, float *iq, std::size_t complex_samples) noexcept;
    // Packing sc16 retains the most significant 12 bits; the bottom 4 are ignored.
    void pack_sc16(const std::int16_t *iq, std::uint8_t *wire, std::size_t complex_samples) noexcept;
    void pack_fc32(const float *iq, std::uint8_t *wire, std::size_t complex_samples) noexcept;

    // Signed8 wire stores I[11:4], Q[11:4]. Conversion preserves the existing
    // packed12/sc16/fc32 meanings; TX first quantizes to 12 bits, then drops 4 LSBs.
    void unpack8_packed12(const std::uint8_t *, std::uint8_t *, std::size_t) noexcept;
    void pack8_packed12(const std::uint8_t *, std::uint8_t *, std::size_t) noexcept;
    void unpack8_sc16(const std::uint8_t *, std::int16_t *, std::size_t) noexcept;
    void pack8_sc16(const std::int16_t *, std::uint8_t *, std::size_t) noexcept;
    void unpack8_fc32(const std::uint8_t *, float *, std::size_t) noexcept;
    void pack8_fc32(const float *, std::uint8_t *, std::size_t) noexcept;

} // namespace och::dsp
