// SPDX-License-Identifier: GPL-3.0-or-later
// Explicitly sends a 1MHz complex tone on TX1 for about one second.
#include <omnicore/och.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>
int main(int argc, char **argv) {
    try {
        och::device dev(argc > 1 ? argv[1] : "");
        auto config = och::default_config();
        config.sample_rate = 30720000;
        config.rx_mask = 0;
        config.tx_mask = 1;
        config.tx_attenuation[0] = 10;
        dev.configure(config);
        auto args = och::stream_args(OCH_TX);
        args.format = OCH_SC16;
        och::stream tx(dev, args);
        // 25 cycles in 768 samples gives exactly 1MHz at 30.72MSPS.
        // The repeat buffer contains whole periods, preserving phase at wrap.
        constexpr size_t n = 768 * 86;
        std::vector<int16_t> iq(n * 2);
        for (size_t k = 0; k < n; ++k) {
            double phase = 6.2831853071795864769 * 25 * (k % 768) / 768;
            iq[k * 2] = static_cast<int16_t>(std::lround(std::cos(phase) * 511)) * 16;
            iq[k * 2 + 1] = static_cast<int16_t>(std::lround(std::sin(phase) * 511)) * 16;
        }
        size_t cursor = 0;
        auto send_some = [&](size_t requested) {
            size_t accepted = 0;
            int rc = tx.send(iq.data() + cursor * 2, std::min(requested, n - cursor), accepted, 1000);
            cursor = (cursor + accepted) % n;
            if (rc != OCH_TIMEOUT) och::check(rc);
            return accepted;
        };
        // Reserve about 32MiB of wire data before start to cover control latency.
        size_t prefill = 32ull * 1048576 / 3, queued = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (queued < prefill) {
            queued += send_some(prefill - queued);
            if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("prefill timeout");
        }
        tx.start();
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < end) send_some(n);
        tx.stop();
        std::cout << "TX finished; transport padding=" << tx.stats().tx_padding_samples << " samples/channel\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
