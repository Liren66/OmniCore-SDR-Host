// MATLAB TX input -> interleaved och samples (MEX_PLAN.md 5.2). Pure C++ on raw pointers, tested
// offline in tests/tx_events_test.cpp.
//
// The input is an N x channels array, column-major; complex data is interleaved (re, im) as in
// the R2018a API, real data has Q = 0. single / double go to fc32 and are limited to +-1 (NaN
// becomes 0); int16 goes to sc16 and int8 to sc8 unchanged. The output is time-major interleaved:
// ch0, ch1, ch0, ch1, ... with I, Q per sample.
#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace omnicore_mex {
    namespace detail {
        inline float limit(double v, size_t &clipped) {
            if (v > 1) {
                ++clipped;
                return 1;
            }
            if (v < -1) {
                ++clipped;
                return -1;
            }
            if (v != v) {
                ++clipped;
                return 0;
            }
            return float(v);
        }
        template<class Out, class In>
        Out convert(In v, size_t &clipped) {
            if constexpr (std::is_floating_point<Out>::value) {
                return limit(double(v), clipped);
            } else {
                (void) clipped;
                return Out(v);
            }
        }
    } // namespace detail

    // Rows [first, first + n) of every column -> n interleaved samples in `out`. Returns how many
    // I or Q values were outside +-1 (or NaN) and were limited; always 0 for integer types.
    template<class Out, class In>
    size_t gather(const In *data, bool complex, size_t rows, size_t channels, size_t first, size_t n, Out *out) {
        size_t clipped = 0;
        for (size_t c = 0; c < channels; ++c) {
            for (size_t k = 0; k < n; ++k) {
                const size_t r = first + k;
                Out *o = out + (k * channels + c) * 2;
                if (complex) {
                    o[0] = detail::convert<Out>(data[(c * rows + r) * 2], clipped);
                    o[1] = detail::convert<Out>(data[(c * rows + r) * 2 + 1], clipped);
                } else {
                    o[0] = detail::convert<Out>(data[c * rows + r], clipped);
                    o[1] = Out(0);
                }
            }
        }
        return clipped;
    }
} // namespace omnicore_mex
