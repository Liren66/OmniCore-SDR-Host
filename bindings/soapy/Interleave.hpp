// och streams are time-major interleaved (ch0, ch1, ch0, ch1, ...); SoapySDR wants one buffer per
// channel. One complex sample is copied as a unit of its size: CS8 = 2 bytes, CS16 = 4,
// CF32 = 8. No SoapySDR or device dependency (tests/interleave_test.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace omnicore_soapy {
    namespace detail {
        // Fixed-size memcpy compiles to a plain load/store and, unlike a cast, is valid for any
        // alignment the application's buffers have (CF32 buffers need not be 8-byte aligned).
        template<size_t B>
        void deinterleave(const void *src, void *const *dst, size_t channels, size_t offset, size_t n) {
            const uint8_t *in = static_cast<const uint8_t *>(src) + offset * channels * B;
            for (size_t c = 0; c < channels; ++c) {
                uint8_t *out = static_cast<uint8_t *>(dst[c]);
                for (size_t k = 0; k < n; ++k) std::memcpy(out + k * B, in + (k * channels + c) * B, B);
            }
        }
        template<size_t B>
        void interleave(const void *const *src, void *dst, size_t channels, size_t n) {
            uint8_t *out = static_cast<uint8_t *>(dst);
            for (size_t c = 0; c < channels; ++c) {
                const uint8_t *in = static_cast<const uint8_t *>(src[c]);
                for (size_t k = 0; k < n; ++k) std::memcpy(out + (k * channels + c) * B, in + k * B, B);
            }
        }
    } // namespace detail

    // Copies samples [offset, offset + n) of an interleaved buffer into dst[0..channels).
    inline void deinterleave(const void *src, void *const *dst, size_t channels, size_t sample_bytes, size_t offset,
                             size_t n) {
        if (channels == 1) {
            std::memcpy(dst[0], static_cast<const uint8_t *>(src) + offset * sample_bytes, n * sample_bytes);
            return;
        }
        switch (sample_bytes) {
        case 2:
            return detail::deinterleave<2>(src, dst, channels, offset, n);
        case 4:
            return detail::deinterleave<4>(src, dst, channels, offset, n);
        case 8:
            return detail::deinterleave<8>(src, dst, channels, offset, n);
        }
        throw std::invalid_argument("unsupported sample size");
    }

    // Interleaves n samples of each src[c] into dst (TX, S3).
    inline void interleave(const void *const *src, void *dst, size_t channels, size_t sample_bytes, size_t n) {
        if (channels == 1) {
            std::memcpy(dst, src[0], n * sample_bytes);
            return;
        }
        switch (sample_bytes) {
        case 2:
            return detail::interleave<2>(src, dst, channels, n);
        case 4:
            return detail::interleave<4>(src, dst, channels, n);
        case 8:
            return detail::interleave<8>(src, dst, channels, n);
        }
        throw std::invalid_argument("unsupported sample size");
    }
} // namespace omnicore_soapy
