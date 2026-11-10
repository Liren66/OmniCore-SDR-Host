// In-memory Link for offline tests: a scripted SDK on the control pipe, a byte source on
// Pipe::sample_in and a byte sink on Pipe::sample_out. Built only into the test targets.
#pragma once
#include "../link.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace och::transport {
    class FakeLink final : public Link {
    public:
        // --- Test side. Every member function below may be called from the test thread at any time.

        // Reply text for a command, or nullopt to use the built-in SDK below. Runs with the
        // link's lock held: it must not call back into the link.
        std::function<std::optional<std::string>(const std::string &command)> on_command;

        // Set before starting a sample worker. Runs on that worker before submitting a slot.
        std::function<void()> before_sample_submit;

        // Commands that are accepted but never answered.
        void mute(const std::string &command);

        // Queues one unsolicited line ("EVT ...") on the control pipe.
        void emit_line(const std::string &line);

        // Appends bytes the device sends on Pipe::sample_in.
        void feed_rx(const std::vector<uint8_t> &bytes);

        // One framed 16 KiB RX block whose payload byte i is uint8_t(seed + i).
        static std::vector<uint8_t> rx_block(uint8_t flags, uint16_t seq, uint16_t valid, uint64_t time, size_t group,
                                             uint8_t seed = 0);

        // Device removal: every pending and later operation fails.
        void unplug();

        // The next SampleQueue::submit() throws.
        void fail_next_submit();

        // Commands received so far, without the "@id " envelope.
        std::vector<std::string> commands() const;

        // Everything written to Pipe::sample_out.
        std::vector<uint8_t> tx_bytes() const;

        // In-flight transfers cancelled by SampleQueue::stop(), and bridge cleanups (vendor 0xd0 value 2).
        unsigned cancelled_transfers() const;

        unsigned transport_resets() const;

        // Reply to system_info?. Change before attaching the link to a Device.
        std::string system_info = "transport=framed16k firmware=FAKE_SDK_1.0 hw_time=1";

        // --- Link
        uint32_t speed_mbps() const override { return 5000; }

        bool has(Pipe) const override { return true; }

        size_t vendor_in(uint8_t request, uint16_t value, uint16_t index, void *data, size_t size) override;

        void vendor_out(uint8_t request, uint16_t value, uint16_t index, const void *data, size_t size) override;

        size_t transfer(Pipe, uint8_t *data, size_t size, uint32_t timeout_ms) override;

        size_t read_control(uint8_t *data, size_t size) override;

        void shutdown_control() noexcept override;

        void reset_sample_pipes() override;

        std::unique_ptr<SampleQueue> open_samples(Pipe, size_t transfer_bytes, unsigned depth) override;

    private:
        class Queue;

        // Built-in SDK: the handshake, clock_chain?, usb_status? and empty "ok" for the rest.
        std::string sdk_reply(const std::string &command);

        void accept_command(const std::string &line);

        mutable std::mutex mutex;
        std::condition_variable changed;
        std::deque<uint8_t> control_in, rx;
        std::vector<uint8_t> tx;
        std::string control_out_partial;
        std::vector<std::string> log;
        std::set<std::string> muted;
        uint32_t sample_rate = 30720000;
        unsigned cancelled = 0, resets = 0;
        bool unplugged = false, control_closed = false, submit_fails = false;
    };
} // namespace och::transport
