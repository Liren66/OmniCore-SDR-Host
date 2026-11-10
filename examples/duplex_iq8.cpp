// SPDX-License-Identifier: GPL-3.0-or-later
// Explicit 2TX2RX signed8 demonstration: RX samples plus zero-IQ TX for one second.
#include <omnicore/och.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <thread>
#include <vector>
struct Producer {
    std::atomic<bool> stop{false};
    std::thread worker;
    std::exception_ptr error;
    void join() {
        stop = true;
        if (worker.joinable()) worker.join();
    }
    ~Producer() { join(); }
};
int main(int argc, char **argv) {
    try {
        och::device dev(argc > 1 ? argv[1] : "");
        auto config = och::default_config();
        config.rx_mask = config.tx_mask = 3;
        config.usb_iq_bits = 8;
        config.sample_rate = 30720000;
        dev.configure(config);
        auto args = och::stream_args(OCH_RX);
        args.format = OCH_SC8;
        och::stream rx(dev, args);
        args.direction = OCH_TX;
        och::stream tx(dev, args);
        constexpr size_t frames = 49152; // I1,Q1,I2,Q2 per time frame, 4 wire bytes.
        std::vector<int8_t> zeros(frames * 4), samples(frames * 4);
        for (unsigned block = 0; block < 4; ++block) {
            size_t accepted = 0;
            och::check(tx.send(zeros.data(), frames, accepted, 1000));
            if (accepted != frames) throw std::runtime_error("incomplete TX prefill");
        }
        Producer producer;
        producer.worker = std::thread([&] {
            try {
                while (!producer.stop) {
                    size_t accepted = 0;
                    const auto rc = tx.send(zeros.data(), frames, accepted, 100);
                    if (rc != OCH_TIMEOUT) och::check(rc);
                }
            } catch (...) {
                producer.error = std::current_exception();
                producer.stop = true;
            }
        });
        rx.start();
        tx.start();
        uint64_t received_total = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < end && !producer.stop) {
            size_t received = 0;
            och_metadata metadata{};
            const auto rc = rx.recv(samples.data(), frames, received, metadata, 100);
            if (rc == OCH_TIMEOUT) continue;
            och::check(rc);
            received_total += received;
            // samples contain signed I1,Q1,I2,Q2; each value represents 12-bit IQ >> 4.
        }
        producer.join();
        tx.stop();
        rx.stop();
        if (producer.error) std::rethrow_exception(producer.error);
        std::cout << "SC8 received " << received_total << " samples/channel, USB bytes=" << rx.stats().usb_bytes
                  << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
