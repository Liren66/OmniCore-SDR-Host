// Offline test of the TX input conversion (bindings/matlab/mex/TxConvert.hpp).
#include "TxConvert.hpp"
#include <cstdio>
#include <cstring>

using namespace omnicore_mex;

namespace {
    int failures = 0;
    void expect(bool ok, const char *what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    }
} // namespace

int main() {
    {
        // Two complex double channels -> interleaved fc32, limited to +-1.
        const double x[] = {0.5, -0.25, 2.0, 0.0, /* ch2 */ -3.0, 1.0, 0.125, -0.5}; // 2 rows x 2 ch
        float out[8];
        const size_t clipped = gather(x, true, 2, 2, 0, 2, out);
        const float want[] = {0.5f, -0.25f, -1.0f, 1.0f, 1.0f, 0.0f, 0.125f, -0.5f};
        expect(std::memcmp(out, want, sizeof want) == 0, "complex double interleave");
        expect(clipped == 2, "two values limited");
    }
    {
        // Real single, one channel, rows 1..2: Q = 0; NaN -> 0 counted.
        const float x[] = {0.1f, 0.2f, 0.0f / 0.0f};
        float out[4];
        const size_t clipped = gather(x, false, 3, 1, 1, 2, out);
        expect(out[0] == 0.2f && out[1] == 0 && out[2] == 0 && out[3] == 0 && clipped == 1, "real single, NaN");
    }
    {
        // int16 passes through unchanged.
        const int16_t x[] = {100, -200, 32767, -32768}; // 1 row x 2 ch complex
        int16_t out[4];
        expect(gather(x, true, 1, 2, 0, 1, out) == 0, "int16 not clipped");
        expect(out[0] == 100 && out[1] == -200 && out[2] == 32767 && out[3] == -32768, "int16 interleave");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("tx_convert: all passed\n");
    return 0;
}
