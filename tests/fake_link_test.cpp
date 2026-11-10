// Offline test of Device / Stream on a FakeLink (no device needed): handshake, command
// replies and failures, device events, framed RX with a gap and a corrupt header, cancelling
// in-flight transfers, framed TX, reconfiguration of a running RX stream, device removal.
#include "test_support.hpp"
#include <future>
using namespace och::test;

namespace {
    // Three 16 KiB device blocks per transfer and ring block: a whole number of 6-byte sample
    // groups (two channels, 12-bit).
    constexpr uint32_t transfer_bytes = 3 * 16384;
    constexpr size_t group = 6;
    constexpr uint16_t block_samples = 2728; // 16368 payload bytes / 6

    och_stream_args stream_args(uint32_t direction, uint32_t bytes = transfer_bytes) {
        och_stream_args a{};
        a.struct_size = sizeof a;
        a.direction = direction;
        a.format = OCH_PACKED12; // the wire bytes, unchanged
        a.transfer_bytes = bytes;
        a.usb_queue_depth = 4;
        a.host_buffer_bytes = 8 * bytes;
        return a;
    }

    struct Received {
        int rc = OCH_OK;
        size_t samples = 0;
        och_metadata meta{};
        std::vector<uint8_t> bytes;
    };

    Received receive(Stream &s, size_t capacity = 20000) {
        Received r;
        r.bytes.resize(capacity * group);
        r.meta.struct_size = sizeof r.meta;
        r.rc = s.recv(r.bytes.data(), capacity, &r.samples, &r.meta, 2000);
        return r;
    }

    void test_handshake() {
        std::puts("[1] handshake");
        Bench b;
        CHECK(b.device.info.usb_speed_mbps == 5000);
        CHECK(std::string(b.device.info.firmware) == "FAKE_SDK_1.0");
        CHECK(b.device.info.hardware_timestamps == 1);
        CHECK(b.device.firmware.mode != OCH_FW_MODE_BOOTLOADER); // the bridge rejects the firmware requests
        const auto sent = b.link->commands();
        CHECK(sent.size() == 3 && sent[0] == "host_protocol?" && sent[1] == "system_info?" && sent[2] == "usb_stop!");
        CHECK(b.link->transport_resets() == 1);
        CHECK(!b.device.transport_dirty);

        // An SDK without the framed transport is refused.
        auto old = std::make_unique<FakeLink>();
        old->system_info = "firmware=OLD";
        och_device d;
        CHECK(code_of([&] { d.attach(std::move(old)); }) == OCH_UNSUPPORTED);
    }

    void test_commands() {
        std::puts("[2] command replies, rejection, timeout");
        Bench b;
        b.link->on_command = [](const std::string &c) -> std::optional<std::string> {
            if (c == "two_lines?") return "a=1\nb=2";
            if (c == "bad=1") return "ERR out of range";
            return std::nullopt;
        };
        CHECK(b.device.command("two_lines?") == "a=1\nb=2\n");
        CHECK(code_of([&] { b.device.command("bad=1"); }) == OCH_PROTOCOL);
        CHECK(b.device.command("still_alive?").empty()); // a rejected command leaves the channel usable
        CHECK(code_of([&] { b.device.command("two\nlines"); }) == OCH_INVALID);

        // No reply: the command times out and the channel stays failed until reopen.
        b.link->mute("silent?");
        CHECK(code_of([&] { b.device.command("silent?", 100); }) == OCH_TIMEOUT);
        CHECK(code_of([&] { b.device.command("host_protocol?"); }) == OCH_IO);
    }

    void test_device_events() {
        std::puts("[3] EVT lines and loss of the control pipe");
        Bench b;
        b.link->emit_line("EVT type=rx_overflow action=waiting_rx_resync");
        och_event e{};
        CHECK(b.wait_event(OCH_EVENT_DEVICE, &e));
        CHECK(std::string(e.message).find("type=rx_overflow") != std::string::npos);
        CHECK(b.device.stats.overflows == 1);

        b.link->unplug();
        CHECK(b.wait_event(OCH_EVENT_TRANSPORT, &e));
        CHECK(std::string(e.message).find("control disconnected") != std::string::npos);
        CHECK(code_of([&] { b.device.command("host_protocol?"); }) == OCH_IO);
    }

    void test_rx() {
        std::puts("[4] framed RX: metadata, device gap, corrupt header, stop with transfers in flight");
        using namespace och::dsp;
        Bench b;
        b.device.configure(default_config());
        CHECK(b.device.configured && b.device.config.sample_rate == 30720000);
        auto *s = b.device.create_stream<och_stream>(stream_args(OCH_RX));
        s->start();
        CHECK(has(b.link->commands(), "usb_data_en=1 0"));

        // Three continuous blocks, then the device reports a gap of 1000 samples.
        const uint64_t t0 = 0x100005000ULL;
        std::vector<uint8_t> stream;
        const auto add = [&](const std::vector<uint8_t> &block) {
            stream.insert(stream.end(), block.begin(), block.end());
        };
        add(FakeLink::rx_block(frame_rx_time_valid | frame_rx_sob, 0, block_samples, t0, group, 1));
        add(FakeLink::rx_block(frame_rx_time_valid, 1, block_samples, t0 + block_samples, group, 2));
        add(FakeLink::rx_block(frame_rx_time_valid, 2, block_samples, t0 + 2 * block_samples, group, 3));
        const uint64_t t1 = t0 + 3 * block_samples + 1000;
        add(FakeLink::rx_block(frame_rx_time_valid | frame_rx_gap, 3, block_samples, t1, group, 4));
        add(FakeLink::rx_block(frame_rx_time_valid | frame_rx_eob, 4, block_samples, t1 + block_samples, group, 5));
        b.link->feed_rx(stream);

        auto r = receive(*s);
        CHECK(r.rc == OCH_OK && r.samples == 3u * block_samples);
        CHECK(r.meta.flags == (OCH_META_START | OCH_META_HAS_TIME));
        CHECK(r.meta.epoch == 1 && r.meta.sample_index == 0 && r.meta.time_ticks == t0);
        CHECK(r.meta.tick_rate == 30720000);
        CHECK(r.bytes[0] == 1 && r.bytes[1] == 2);                               // block 0 payload
        CHECK(r.bytes[size_t(block_samples) * group] == 2);                      // block 1 follows without a seam
        CHECK(r.bytes[2 * size_t(block_samples) * group + 5] == uint8_t(3 + 5)); // block 2

        r = receive(*s);
        CHECK(r.rc == OCH_OK && r.samples == 2u * block_samples);
        CHECK(r.meta.flags == (OCH_META_START | OCH_META_DISCONTINUITY | OCH_META_DEVICE_OVERFLOW | OCH_META_HAS_TIME |
                               OCH_META_END_OF_BURST));
        CHECK(r.meta.epoch == 2 && r.meta.sample_index == 0 && r.meta.time_ticks == t1);
        CHECK(r.meta.device_dropped_samples == 1000);
        CHECK(b.device.stats.overflows == 1);

        // A corrupt header: the parser drops that block and realigns on the next one.
        auto bad = FakeLink::rx_block(frame_rx_time_valid, 5, block_samples, t1 + 2 * block_samples, group);
        bad[0] ^= 0x55;
        b.link->feed_rx(bad);
        const uint64_t t2 = t1 + 3 * block_samples;
        b.link->feed_rx(FakeLink::rx_block(frame_rx_time_valid | frame_rx_eob, 6, 100, t2, group, 9));
        r = receive(*s);
        CHECK(r.rc == OCH_OK && r.samples == 100 && r.meta.time_ticks == t2 && r.bytes[0] == 9);
        CHECK((r.meta.flags & OCH_META_DISCONTINUITY) && !(r.meta.flags & OCH_META_DEVICE_OVERFLOW));
        och_stats st{};
        st.struct_size = sizeof st;
        s->fill_stats(&st);
        CHECK(st.resyncs == 1 && st.headers == 6 && st.host_dropped_samples == 0 && st.transfer_errors == 0);
        CHECK(st.usb_bytes == 7 * frame_block_bytes && st.usb_queue_pending == 4);

        // Nothing more arrives: stop cancels the four transfers in flight.
        s->stop(0);
        CHECK(b.link->cancelled_transfers() == 4 && s->pending == 0 && !s->running);
        CHECK(has(b.link->commands(), "usb_data_en=0 0") && b.device.transport_dirty);
        CHECK(receive(*s).rc == OCH_STOPPED);
        b.device.destroy_stream(s);
        CHECK(b.device.rx == nullptr);
    }

    void test_rx_burst_end() {
        std::puts("[RX] burst end at an exact block boundary and after a partial read");
        using namespace och::dsp;
        Bench b;
        b.device.configure(default_config());
        auto args = stream_args(OCH_RX, frame_block_bytes);
        args.start_flags = OCH_RX_TIMED;
        auto *s = b.device.create_stream<och_stream>(args);
        s->start();

        constexpr uint64_t t0 = 1000;
        const auto remainder = uint16_t(frame_block_bytes / group - block_samples);
        b.link->feed_rx(FakeLink::rx_block(frame_rx_time_valid | frame_rx_sob, 0, block_samples, t0, group));
        b.link->feed_rx(FakeLink::rx_block(frame_rx_time_valid | frame_rx_eob, 1, remainder, t0 + block_samples, group));
        auto r = receive(*s);
        CHECK(r.rc == OCH_OK && r.samples == frame_block_bytes / group && (r.meta.flags & OCH_META_END_OF_BURST));

        constexpr uint64_t t1 = 5000;
        b.link->feed_rx(FakeLink::rx_block(frame_rx_time_valid | frame_rx_sob | frame_rx_eob, 2, 4, t1, group));
        r = receive(*s, 2);
        CHECK(r.rc == OCH_OK && r.samples == 2 && r.meta.time_ticks == t1 && !(r.meta.flags & OCH_META_END_OF_BURST));
        r = receive(*s, 2);
        CHECK(r.rc == OCH_OK && r.samples == 2 && r.meta.time_ticks == t1 + 2 && (r.meta.flags & OCH_META_END_OF_BURST));
        b.device.destroy_stream(s);
    }

    void test_tx() {
        std::puts("[5] framed TX: block headers, burst end, payload");
        using namespace och::dsp;
        Bench b;
        auto c = default_config();
        c.rx_mask = 0;
        c.tx_mask = 3;
        b.device.configure(c);
        auto *s = b.device.create_stream<och_stream>(stream_args(OCH_TX));
        CHECK(code_of([&] { s->start(); }) == OCH_BUSY); // nothing queued yet

        const size_t count = 5000; // one full block and a partial one
        std::vector<uint8_t> samples(count * group);
        for (size_t i = 0; i < samples.size(); ++i) samples[i] = uint8_t(i * 7 + 1);
        size_t accepted = 0;
        CHECK(s->send(samples.data(), count, 0, 0, &accepted, 1000) == OCH_OK && accepted == count);
        s->start();
        CHECK(has(b.link->commands(), "tx_enable=1"));
        s->flush(2000);
        s->stop(0);

        const auto sent = b.link->tx_bytes();
        CHECK(sent.size() == 2 * frame_block_bytes);
        FrameHeader h0, h1;
        CHECK(sent.size() >= 2 * frame_block_bytes && parse_frame_header(sent.data(), h0) &&
              parse_frame_header(sent.data() + frame_block_bytes, h1));
        CHECK(h0.seq == 0 && h0.valid == block_samples && (h0.flags & frame_tx_sob) && !(h0.flags & frame_tx_eob));
        CHECK(h1.seq == 1 && h1.valid == count - block_samples && (h1.flags & frame_tx_eob));
        if (sent.size() == 2 * frame_block_bytes) {
            CHECK(!memcmp(sent.data() + frame_header_bytes, samples.data(), frame_payload_bytes));
            CHECK(!memcmp(sent.data() + frame_block_bytes + frame_header_bytes, samples.data() + frame_payload_bytes,
                          samples.size() - frame_payload_bytes));
        }
        och_stats st{};
        st.struct_size = sizeof st;
        s->fill_stats(&st);
        CHECK(st.usb_bytes == 2 * frame_block_bytes && st.tx_padding_samples == 2 * block_samples - count);
        b.device.destroy_stream(s);
    }

    void test_tx_flush_during_submit() {
        std::puts("[TX] flush waits for a block being submitted");
        Bench b;
        auto c = default_config();
        c.rx_mask = 0;
        c.tx_mask = 3;
        b.device.configure(c);
        auto *s = b.device.create_stream<och_stream>(stream_args(OCH_TX));

        std::atomic<bool> pause_next_submit{false};
        std::promise<void> entered, resume;
        auto submitting = entered.get_future();
        auto resumed = resume.get_future();
        b.link->before_sample_submit = [&] {
            if (pause_next_submit.exchange(false)) {
                entered.set_value();
                resumed.wait_for(std::chrono::seconds(2));
            }
        };

        const std::vector<uint8_t> samples(block_samples * group, 0x5a);
        size_t accepted = 0;
        CHECK(s->send(samples.data(), block_samples, OCH_TX_END_OF_BURST, 0, &accepted, 1000) == OCH_OK);
        s->start();
        s->flush(2000);
        pause_next_submit = true;
        accepted = 0;
        CHECK(s->send(samples.data(), 1, OCH_TX_END_OF_BURST, 0, &accepted, 1000) == OCH_OK && accepted == 1);
        CHECK(submitting.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        CHECK(code_of([&] { s->flush(0); }) == OCH_TIMEOUT);
        resume.set_value();
        s->flush(2000);
        CHECK(b.link->tx_bytes().size() == 2 * och::dsp::frame_block_bytes);
        b.device.destroy_stream(s);
    }

    void test_reconfigure() {
        std::puts("[6] reconfiguration of a running RX stream");
        using namespace och::dsp;
        Bench b;
        b.device.configure(default_config());
        auto *s = b.device.create_stream<och_stream>(stream_args(OCH_RX));
        s->start();
        b.link->feed_rx(FakeLink::rx_block(frame_rx_time_valid | frame_rx_sob | frame_rx_eob, 0, 500, 1000, group));
        auto r = receive(*s);
        CHECK(r.rc == OCH_OK && r.samples == 500 && r.meta.epoch == 1);
        b.events();

        // A gain mode change stops the stream, applies and resumes it; the next block is marked.
        auto c = b.device.config;
        c.rx_gain[0] = 20;
        c.rx_agc = 0;
        const auto resets = b.link->transport_resets();
        b.device.configure(c);
        CHECK(b.link->transport_resets() == resets + 1);
        och_event e{};
        CHECK(b.wait_event(OCH_EVENT_RECONFIGURED, &e));
        CHECK(std::string(e.message).find("reconfigure complete rx_resumed=1") != std::string::npos);
        CHECK(s->running && s->started);
        b.link->feed_rx(FakeLink::rx_block(frame_rx_time_valid | frame_rx_sob | frame_rx_eob, 0, 300, 9000, group));
        r = receive(*s);
        CHECK(r.rc == OCH_OK && r.samples == 300 && r.meta.epoch == 2 && r.meta.time_ticks == 9000);
        CHECK((r.meta.flags & OCH_META_RECONFIGURED) && (r.meta.flags & OCH_META_DISCONTINUITY));

        c.rx_gain[0] = 25; // manual gain only: retune without stopping
        b.link->feed_rx(FakeLink::rx_block(frame_rx_time_valid | frame_rx_eob, 1, 100, 9300, group));
        const void *data = nullptr;
        size_t bytes = 0;
        och_metadata lease_meta{};
        lease_meta.struct_size = sizeof lease_meta;
        const int acquired = s->rx_acquire(&data, &bytes, &lease_meta, 1000);
        CHECK(acquired == OCH_OK);
        if (acquired == OCH_OK) {
            const auto commands = b.link->commands().size();
            CHECK(code_of([&] { b.device.configure(c); }) == OCH_BUSY);
            CHECK(b.link->commands().size() == commands);
            s->rx_release();
        }
        b.device.configure(c);
        CHECK(b.link->transport_resets() == resets + 1);
        CHECK(b.wait_event(OCH_EVENT_RECONFIGURED, &e));
        CHECK(std::string(e.message).find("retune complete streams_continued=1") != std::string::npos);

        // A new sample rate that the SDK does not apply fails the transaction and stops the stream.
        b.link->on_command = [](const std::string &cmd) -> std::optional<std::string> {
            if (cmd.rfind("rx_samp_freq=", 0) == 0) return ""; // accepted, but the clock chain keeps the old rate
            return std::nullopt;
        };
        c.sample_rate = 15360000;
        CHECK(code_of([&] { b.device.configure(c); }) == OCH_PROTOCOL);
        CHECK(!b.device.configured && !s->running);
        CHECK(b.wait_event(OCH_EVENT_TRANSPORT, &e));
        CHECK(std::string(e.message).find("reconfigure failed") != std::string::npos);
        std::string why;
        CHECK(code_of([&] { receive(*s); }, &why) == OCH_IO && why.find("reconfiguration failed") != std::string::npos);
        b.device.destroy_stream(s);
    }

    void test_unplug() {
        std::puts("[7] device removal while streaming, failed submission");
        Bench b;
        b.device.configure(default_config());
        auto *s = b.device.create_stream<och_stream>(stream_args(OCH_RX));
        s->start();
        b.link->unplug();
        och_event e{};
        bool stream_failed = false;
        for (unsigned i = 0; i < 4 && !stream_failed; ++i)
            if (b.wait_event(OCH_EVENT_TRANSPORT, &e))
                stream_failed = std::string(e.message).find("USB transfer failed") != std::string::npos;
        CHECK(stream_failed);
        std::string why;
        CHECK(code_of([&] { receive(*s); }, &why) == OCH_IO && why.find("device unplugged") != std::string::npos);
        // The stop command cannot reach the device; the stream is still torn down and freed.
        CHECK(code_of([&] { b.device.destroy_stream(s); }) == OCH_IO);
        CHECK(b.device.rx == nullptr);

        Bench second;
        second.device.configure(default_config());
        auto *t = second.device.create_stream<och_stream>(stream_args(OCH_RX));
        second.link->fail_next_submit();
        CHECK(code_of([&] { t->start(); }, &why) == OCH_IO &&
              why.find("submission failed: injected") != std::string::npos);
        CHECK(!t->running && second.device.transport_dirty);
        second.device.destroy_stream(t);
    }

    // 16384 is not a multiple of the 6-byte sample group: ring blocks carry 2730 whole samples.
    void test_uneven_transfer_size() {
        std::puts("[8] transfer size that is not a whole number of sample groups");
        using namespace och::dsp;
        {
            Bench b;
            b.device.configure(default_config());
            auto *s = b.device.create_stream<och_stream>(stream_args(OCH_RX, 16384));
            s->start();
            const uint64_t t0 = 5000;
            for (uint16_t i = 0; i < 4; ++i)
                b.link->feed_rx(
                        FakeLink::rx_block(frame_rx_time_valid | (i ? 0 : frame_rx_sob) | (i == 3 ? frame_rx_eob : 0),
                                           i, block_samples, t0 + i * uint64_t(block_samples), group));
            size_t total = 0;
            bool end = false;
            while (!end && total < 4u * block_samples) {
                const auto r = receive(*s);
                CHECK(r.rc == OCH_OK && r.samples > 0 && r.samples <= 16384 / group);
                if (r.rc != OCH_OK) break;
                CHECK(r.meta.epoch == 1 && r.meta.sample_index == total && r.meta.time_ticks == t0 + total);
                total += r.samples;
                end = (r.meta.flags & OCH_META_END_OF_BURST) != 0;
            }
            CHECK(total == 4u * block_samples && end);
            s->stop(0); // used to hang: the worker never left rx_frame()
            b.device.destroy_stream(s);
        }
        {
            // TX restart prefill: four ring blocks of zeros are whole samples too.
            Bench b;
            auto c = default_config();
            c.rx_mask = 0;
            c.tx_mask = 3;
            b.device.configure(c);
            auto *s = b.device.create_stream<och_stream>(stream_args(OCH_TX, 16384));
            const std::vector<uint8_t> samples(100 * group, 0x5a);
            size_t accepted = 0;
            CHECK(s->send(samples.data(), 100, 0, 0, &accepted, 1000) == OCH_OK);
            s->start();
            s->flush(2000);
            c.tx_bandwidth = 10000000;
            b.device.configure(c);
            och_event e{};
            CHECK(b.wait_event(OCH_EVENT_RECONFIGURED, &e));
            const uint64_t zeros = 4 * (16384 / group);
            CHECK(std::string(e.message).find("tx_resumed=1") != std::string::npos);
            CHECK(std::string(e.message).find("tx_padding_samples=" + std::to_string(zeros) + " ") !=
                  std::string::npos);
            s->flush(2000);
            s->stop(0);
            const auto sent = b.link->tx_bytes();
            uint64_t valid = 0;
            FrameHeader h;
            for (size_t at = frame_block_bytes; at + frame_block_bytes <= sent.size(); at += frame_block_bytes) {
                CHECK(parse_frame_header(sent.data() + at, h));
                valid += h.valid;
            }
            CHECK(sent.size() % frame_block_bytes == 0 && valid == zeros);
            b.device.destroy_stream(s);
        }
    }
} // namespace

int main() {
    test_handshake();
    test_commands();
    test_device_events();
    test_rx();
    test_rx_burst_end();
    test_tx();
    test_tx_flush_during_submit();
    test_reconfigure();
    test_unplug();
    test_uneven_transfer_size();
    std::printf("FAKE LINK TEST: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
