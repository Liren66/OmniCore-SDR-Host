// SPDX-License-Identifier: GPL-3.0-or-later
// RX only: one consumer and one management thread (main). No implicit TX.
// Requires OmniCoreHost 0.3+. Existing firmware/FIR constraints still apply.
#include <omnicore/och.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <thread>
#include <vector>

int main(int argc, char **argv) {
    try {
        och::device dev(argc > 1 ? argv[1] : "");
        auto config = och::default_config();
        config.sample_rate = 30720000;
        config.rx_mask = 1;
        config.tx_mask = 0;
        config.rx_frequency = 100000000;
        config.rx_bandwidth = 10000000;
        config.rx_agc = 0;
        config.rx_gain[0] = 20.0;
        dev.configure(config);

        auto args = och::stream_args(OCH_RX);
        args.format = OCH_FC32;
        och::stream rx(dev, args);
        // One channel, I/Q floats; counts passed to recv are complex samples.
        std::vector<float> samples(65536 * 2);
        std::atomic<bool> done{false};
        std::exception_ptr consumer_error;
        uint64_t received_total = 0, new_segments = 0, reconfigured_segments = 0;
        rx.start();

        std::thread consumer([&] {
            try {
                while (!done.load()) {
                    size_t received = 0;
                    och_metadata meta{};
                    int rc = rx.recv(samples.data(), 65536, received, meta, 250);
                    // The same stream remains valid during coordinated reconfiguration.
                    // Reconfiguration can return TIMEOUT immediately; back off
                    // briefly so retries do not spin on the stream IO mutex.
                    if (rc == OCH_TIMEOUT) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    if (rc == OCH_STOPPED && done.load()) break;
                    och::check(rc);
                    if (meta.flags & (OCH_META_START | OCH_META_DISCONTINUITY)) {
                        ++new_segments;
                        // Reset downstream filter/demodulator state at this boundary.
                    }
                    if (meta.flags & OCH_META_RECONFIGURED) ++reconfigured_segments;
                    received_total += received;
                    // Perform bounded-time processing here. Do not print every IQ block.
                    // A real decoder also needs the updated rate/frequency at the boundary;
                    // och_metadata does not carry a complete RF configuration snapshot.
                }
            } catch (...) {
                consumer_error = std::current_exception();
                done.store(true);
            }
        });

        std::exception_ptr management_error;
        try {
            // Drain events on the management thread, keeping the consumer uninterrupted.
            auto receive_for = [&](std::chrono::seconds duration) {
                auto until = std::chrono::steady_clock::now() + duration;
                while (!done.load() && std::chrono::steady_clock::now() < until) {
                    och_event event{};
                    // Bound work per iteration, even if device events are frequent.
                    for (unsigned i = 0; i < 16 && dev.poll_event(event, 0); ++i) {
                        if (event.type == OCH_EVENT_RECONFIGURED) std::cout << event.message << '\n';
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            };

            receive_for(std::chrono::seconds(2));
            if (!done.load()) {
                // Masks and stream format stay fixed. One transaction changes LO and gain.
                config = dev.config();
                config.rx_frequency = 100100000;
                config.rx_gain[0] = 25.0;
                dev.configure(config);
                std::cout << "RX frequency=100.1 MHz, gain=25 dB\n";
            }
            receive_for(std::chrono::seconds(2));
            if (!done.load()) {
                // Shared RX/TX sample clock; selected FIR must allow this rate.
                dev.set_sample_rate(15360000);
                std::cout << "Shared sample rate=15.36 MSPS\n";
            }
            receive_for(std::chrono::seconds(2));
        } catch (...) {
            management_error = std::current_exception();
        }

        // Always stop and join before destroying the stream, including on configuration
        // failure. Never let an exception unwind through a joinable std::thread.
        done.store(true);
        try {
            rx.stop();
        } catch (...) {
            if (!management_error) management_error = std::current_exception();
        }
        consumer.join();
        if (management_error) std::rethrow_exception(management_error);
        if (consumer_error) std::rethrow_exception(consumer_error);

        auto stats = rx.stats();
        std::cout << "Received " << received_total << " samples/channel, segments=" << new_segments
                  << ", reconfigured segments=" << reconfigured_segments
                  << ", known host loss=" << stats.host_dropped_samples << '\n';
        rx.close();
        dev.close();
        return 0;
    } catch (const std::exception &e) {
        // A failed hardware mutation needs explicit recovery/configuration and new streams;
        // this example exits without retrying or restarting any transmitter.
        std::cerr << e.what() << '\n';
        return 1;
    }
}
