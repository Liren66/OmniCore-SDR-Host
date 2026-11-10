#include "codec.hpp"

#include <cmath>
#include <cstring>

#if !defined(OCH_FORCE_SCALAR) && (defined(__GNUC__) || defined(__clang__)) &&                                         \
        (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define OCH_TARGET_AVX2 __attribute__((target("avx2")))
#define OCH_HAS_AVX2_IMPL 1
#else
#define OCH_HAS_AVX2_IMPL 0
#endif

namespace och::dsp {
    namespace {

        inline std::int16_t decode12(unsigned value) noexcept {
            const int signed_value = value >= 2048 ? static_cast<int>(value) - 4096 : static_cast<int>(value);
            return static_cast<std::int16_t>(signed_value * 16);
        }

        inline int quantize(float value) noexcept {
            if (std::isnan(value)) return 0;
            if (value >= 2047.0f / 2048.0f) return 2047;
            if (value <= -1.0f) return -2048;
            const float scaled = value * 2048.0f;
            const int whole = static_cast<int>(scaled);
            const float fraction = scaled - static_cast<float>(whole);
            return whole + (fraction >= 0.5f ? 1 : (fraction <= -0.5f ? -1 : 0));
        }

        inline void write12(unsigned i, unsigned q, std::uint8_t *wire) noexcept {
            wire[0] = static_cast<std::uint8_t>(i >> 4);
            wire[1] = static_cast<std::uint8_t>((i << 4) | ((q >> 8) & 15));
            wire[2] = static_cast<std::uint8_t>(q);
        }

#if OCH_HAS_AVX2_IMPL
        // Eight complex samples = 24 bytes. Both unaligned 16-byte loads are wholly
        // inside this span, so no speculative tail read or input padding is required.
        OCH_TARGET_AVX2 inline __m256i decode8(const std::uint8_t *p) noexcept {
            const auto lo = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p));
            const auto hi = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p + 8));
            const auto in = _mm256_inserti128_si256(_mm256_castsi128_si256(lo), hi, 1);
            const auto order = _mm256_setr_epi8(1, 0, 2, 1, 4, 3, 5, 4, 7, 6, 8, 7, 10, 9, 11, 10, 5, 4, 6, 5, 8, 7, 9,
                                                8, 11, 10, 12, 11, 14, 13, 15, 14);
            const auto raw = _mm256_shuffle_epi8(in, order);
            return _mm256_blend_epi16(_mm256_and_si256(raw, _mm256_set1_epi16(static_cast<short>(-16))),
                                      _mm256_slli_epi16(raw, 4), 0xaa);
        }

        OCH_TARGET_AVX2 inline void encode8(__m256i in, std::uint8_t *p) noexcept {
            const auto a_order = _mm256_setr_epi8(1, 0, -1, 5, 4, -1, 9, 8, -1, 13, 12, -1, -1, -1, -1, -1, 1, 0, -1, 5,
                                                  4, -1, 9, 8, -1, 13, 12, -1, -1, -1, -1, -1);
            const auto b_order = _mm256_setr_epi8(-1, 3, 2, -1, 7, 6, -1, 11, 10, -1, 15, 14, -1, -1, -1, -1, -1, 3, 2,
                                                  -1, 7, 6, -1, 11, 10, -1, 15, 14, -1, -1, -1, -1);
            const auto a_mask = _mm256_setr_epi8(-1, -16, 0, -1, -16, 0, -1, -16, 0, -1, -16, 0, 0, 0, 0, 0, -1, -16, 0,
                                                 -1, -16, 0, -1, -16, 0, -1, -16, 0, 0, 0, 0, 0);
            const auto packed = _mm256_or_si256(_mm256_and_si256(_mm256_shuffle_epi8(in, a_order), a_mask),
                                                _mm256_shuffle_epi8(_mm256_srli_epi16(in, 4), b_order));
            const auto lo = _mm256_castsi256_si128(packed);
            const auto hi = _mm256_extracti128_si256(packed, 1);
            const auto l0 = static_cast<std::uint64_t>(_mm_cvtsi128_si64(lo));
            const auto l1 = static_cast<std::uint32_t>(_mm_extract_epi32(lo, 2));
            const auto h0 = static_cast<std::uint64_t>(_mm_cvtsi128_si64(hi));
            const auto h1 = static_cast<std::uint32_t>(_mm_extract_epi32(hi, 2));
            std::memcpy(p, &l0, 8);
            std::memcpy(p + 8, &l1, 4);
            std::memcpy(p + 12, &h0, 8);
            std::memcpy(p + 20, &h1, 4);
        }

        OCH_TARGET_AVX2 inline __m256i quantize8(__m256 value) noexcept {
            value = _mm256_andnot_ps(_mm256_cmp_ps(value, value, _CMP_UNORD_Q), value);
            value = _mm256_mul_ps(value, _mm256_set1_ps(2048.0f));
            value = _mm256_max_ps(_mm256_set1_ps(-2048.0f), _mm256_min_ps(_mm256_set1_ps(2047.0f), value));
            // Do not add +/-0.5 before conversion: floating-point rounding would turn
            // nextafter(0.5, 0) into 1.0 and round the immediately smaller value wrong.
            const auto whole = _mm256_cvttps_epi32(value);
            const auto fraction = _mm256_sub_ps(value, _mm256_cvtepi32_ps(whole));
            const auto absolute_fraction = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), fraction);
            const auto round_up =
                    _mm256_castps_si256(_mm256_cmp_ps(absolute_fraction, _mm256_set1_ps(0.5f), _CMP_GE_OQ));
            const auto direction =
                    _mm256_or_si256(_mm256_srai_epi32(_mm256_castps_si256(value), 31), _mm256_set1_epi32(1));
            return _mm256_add_epi32(whole, _mm256_and_si256(round_up, direction));
        }

        OCH_TARGET_AVX2 std::size_t unpack_s16_avx(const std::uint8_t *p, std::int16_t *out, std::size_t n) noexcept {
            std::size_t k = 0;
            for (; n - k >= 8; k += 8)
                _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + 2 * k), decode8(p + 3 * k));
            return k;
        }
        OCH_TARGET_AVX2 std::size_t unpack_f32_avx(const std::uint8_t *p, float *out, std::size_t n) noexcept {
            std::size_t k = 0;
            const auto scale = _mm256_set1_ps(1.0f / 32768.0f);
            for (; n - k >= 8; k += 8) {
                const auto samples = decode8(p + 3 * k);
                const auto lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(samples));
                const auto hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(samples, 1));
                _mm256_storeu_ps(out + 2 * k, _mm256_mul_ps(_mm256_cvtepi32_ps(lo), scale));
                _mm256_storeu_ps(out + 2 * k + 8, _mm256_mul_ps(_mm256_cvtepi32_ps(hi), scale));
            }
            return k;
        }
        OCH_TARGET_AVX2 std::size_t pack_s16_avx(const std::int16_t *p, std::uint8_t *out, std::size_t n) noexcept {
            std::size_t k = 0;
            for (; n - k >= 8; k += 8)
                encode8(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(p + 2 * k)), out + 3 * k);
            return k;
        }
        OCH_TARGET_AVX2 std::size_t pack_f32_avx(const float *p, std::uint8_t *out, std::size_t n) noexcept {
            std::size_t k = 0;
            for (; n - k >= 8; k += 8) {
                const auto lo = quantize8(_mm256_loadu_ps(p + 2 * k));
                const auto hi = quantize8(_mm256_loadu_ps(p + 2 * k + 8));
                const auto words = _mm256_permute4x64_epi64(_mm256_packs_epi32(lo, hi), 0xd8);
                encode8(_mm256_slli_epi16(words, 4), out + 3 * k);
            }
            return k;
        }
#endif
    } // namespace

    std::size_t sample_bytes(Format format) noexcept {
        switch (format) {
        case Format::packed12:
            return 3;
        case Format::sc16:
            return 4;
        case Format::fc32:
            return 8;
        }
        return 0;
    }

    bool avx2_available() noexcept {
#if OCH_HAS_AVX2_IMPL
        static const bool supported = [] {
            __builtin_cpu_init();
            return __builtin_cpu_supports("avx2") != 0;
        }();
        return supported;
#else
        return false;
#endif
    }
    const char *implementation_name() noexcept { return avx2_available() ? "avx2" : "scalar"; }

    void unpack_sc16(const std::uint8_t *p, std::int16_t *out, std::size_t n) noexcept {
        std::size_t k = 0;
#if OCH_HAS_AVX2_IMPL
        if (avx2_available()) k = unpack_s16_avx(p, out, n);
#endif
        for (; k < n; ++k) {
            out[2 * k] = decode12((static_cast<unsigned>(p[3 * k]) << 4) | (p[3 * k + 1] >> 4));
            out[2 * k + 1] = decode12(((p[3 * k + 1] & 15u) << 8) | p[3 * k + 2]);
        }
    }
    void unpack_fc32(const std::uint8_t *p, float *out, std::size_t n) noexcept {
        std::size_t k = 0;
#if OCH_HAS_AVX2_IMPL
        if (avx2_available()) k = unpack_f32_avx(p, out, n);
#endif
        for (; k < n; ++k) {
            out[2 * k] = decode12((static_cast<unsigned>(p[3 * k]) << 4) | (p[3 * k + 1] >> 4)) / 32768.0f;
            out[2 * k + 1] = decode12(((p[3 * k + 1] & 15u) << 8) | p[3 * k + 2]) / 32768.0f;
        }
    }
    void pack_sc16(const std::int16_t *p, std::uint8_t *out, std::size_t n) noexcept {
        std::size_t k = 0;
#if OCH_HAS_AVX2_IMPL
        if (avx2_available()) k = pack_s16_avx(p, out, n);
#endif
        for (; k < n; ++k)
            write12(static_cast<std::uint16_t>(p[2 * k]) >> 4, static_cast<std::uint16_t>(p[2 * k + 1]) >> 4,
                    out + 3 * k);
    }
    void pack_fc32(const float *p, std::uint8_t *out, std::size_t n) noexcept {
        std::size_t k = 0;
#if OCH_HAS_AVX2_IMPL
        if (avx2_available()) k = pack_f32_avx(p, out, n);
#endif
        for (; k < n; ++k)
            write12(static_cast<unsigned>(quantize(p[2 * k])) & 4095u,
                    static_cast<unsigned>(quantize(p[2 * k + 1])) & 4095u, out + 3 * k);
    }

    void unpack8_packed12(const std::uint8_t *p, std::uint8_t *out, std::size_t n) noexcept {
        for (std::size_t k = 0; k < n; ++k) {
            out[3 * k] = p[2 * k];
            out[3 * k + 1] = p[2 * k + 1] >> 4;
            out[3 * k + 2] = static_cast<std::uint8_t>(p[2 * k + 1] << 4);
        }
    }
    void pack8_packed12(const std::uint8_t *p, std::uint8_t *out, std::size_t n) noexcept {
        for (std::size_t k = 0; k < n; ++k) {
            out[2 * k] = p[3 * k];
            out[2 * k + 1] = static_cast<std::uint8_t>((p[3 * k + 1] << 4) | (p[3 * k + 2] >> 4));
        }
    }
    void unpack8_sc16(const std::uint8_t *p, std::int16_t *out, std::size_t n) noexcept {
        for (std::size_t k = 0; k < 2 * n; ++k)
            out[k] = static_cast<std::int16_t>((p[k] < 128 ? int(p[k]) : int(p[k]) - 256) * 256);
    }
    void pack8_sc16(const std::int16_t *p, std::uint8_t *out, std::size_t n) noexcept {
        for (std::size_t k = 0; k < 2 * n; ++k)
            out[k] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(p[k]) >> 8);
    }
    void unpack8_fc32(const std::uint8_t *p, float *out, std::size_t n) noexcept {
        for (std::size_t k = 0; k < 2 * n; ++k) out[k] = (p[k] < 128 ? int(p[k]) : int(p[k]) - 256) / 128.0f;
    }
    void pack8_fc32(const float *p, std::uint8_t *out, std::size_t n) noexcept {
        for (std::size_t k = 0; k < 2 * n; ++k)
            out[k] = static_cast<std::uint8_t>((static_cast<unsigned>(quantize(p[k])) & 4095u) >> 4);
    }

} // namespace och::dsp
