// SPDX-License-Identifier: GPL-3.0-or-later
// Single consumer, blocking receives. Output is deliberately not written to a slow console.
#include <omnicore/och.hpp>
#include <cstdint>
#include <iostream>
#include <vector>

int main(int argc, char **argv) {
    try {
        och::device dev(argc > 1 ? argv[1] : "");
        auto config = och::default_config();
        config.sample_rate = 30720000;
        config.rx_mask = 3;
        config.tx_mask = 0;
        dev.configure(config);
        auto args = och::stream_args(OCH_RX);
        args.format = OCH_SC16;
        och::stream rx(dev, args);
        // Two channels, each sample has I and Q; capacity is samples PER CHANNEL.
        std::vector<int16_t> samples(65536 * 2 * 2);
        rx.start();
        uint64_t total = 0;
        while (total < config.sample_rate) {
            size_t received = 0;
            och_metadata meta{};
            int rc = rx.recv(samples.data(), 65536, received, meta, 1000);
            if (rc == OCH_TIMEOUT) continue;
            och::check(rc);
            if (meta.flags & (OCH_META_START | OCH_META_DISCONTINUITY))
                std::cerr << "new segment epoch=" << meta.epoch << " index=" << meta.sample_index
                          << " lost_on_device=" << meta.device_dropped_samples << '\n';
            // samples: I1,Q1,I2,Q2,I1,Q1,I2,Q2,...; sc16 is signed 12-bit << 4.
            // Replace this comment with one bounded-time consumer or a downstream queue.
            total += received;
        }
        rx.stop();
        auto stats = rx.stats();
        std::cout << "received " << total << " samples/channel, USB bytes=" << stats.usb_bytes
                  << ", host loss=" << stats.host_dropped_samples << '\n';
        // rx is destroyed first, then dev. Do not let a stream outlive its device.
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
