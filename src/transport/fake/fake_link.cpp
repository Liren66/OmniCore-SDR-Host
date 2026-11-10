#include "fake_link.hpp"
#include "../../protocol/frame.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace och::transport {
    namespace {
        const char *const gone = "completion: device unplugged";
    }

    // Transfers complete in submission order: an RX transfer as soon as any fed bytes are
    // available (a short read, like the real pipe), a TX transfer at once.
    class FakeLink::Queue final : public SampleQueue {
    public:
        Queue(FakeLink &l, bool in, size_t transfer_bytes, unsigned depth) : link(l), receive(in), slots(depth) {
            for (auto &s: slots) {
                s.bytes.assign(transfer_bytes, 0);
                s.data = s.bytes.data();
                s.capacity = transfer_bytes;
            }
        }

        size_t size() const override { return slots.size(); }

        Transfer &slot(size_t index) override { return slots[index]; }

        size_t max_transfer_bytes() override { return 1048576; }

        void start() override {}

        void submit(Transfer &t) override {
            if (link.before_sample_submit) link.before_sample_submit();
            std::lock_guard<std::mutex> lock(link.mutex);
            if (link.unplugged) throw Error("USB streaming submission failed: device unplugged");
            if (link.submit_fails) {
                link.submit_fails = false;
                throw Error("USB streaming submission failed: injected");
            }
            in_flight.push_back(&t);
        }

        Wait wait(uint32_t timeout_ms, Transfer *&done, size_t &bytes) override {
            std::unique_lock<std::mutex> lock(link.mutex);
            const auto ready = [&] { return woken || link.unplugged || !receive || !link.rx.empty(); };
            if (!link.changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) return Wait::timeout;
            if (woken) return Wait::woken;
            if (link.unplugged) throw Error("USB transfer failed: device unplugged");
            Transfer *t = in_flight.front();
            if (receive) {
                bytes = std::min(t->used, link.rx.size());
                std::copy_n(link.rx.begin(), bytes, t->data);
                link.rx.erase(link.rx.begin(), link.rx.begin() + static_cast<std::ptrdiff_t>(bytes));
            } else {
                bytes = t->used;
                link.tx.insert(link.tx.end(), t->data, t->data + bytes);
            }
            in_flight.pop_front();
            done = t;
            return Wait::completed;
        }

        void wake() noexcept override {
            std::lock_guard<std::mutex> lock(link.mutex);
            woken = true;
            link.changed.notify_all();
        }

        uint64_t stop() override {
            std::lock_guard<std::mutex> lock(link.mutex);
            uint64_t unconfirmed = 0;
            for (const Transfer *t: in_flight) unconfirmed += t->used;
            link.cancelled += static_cast<unsigned>(in_flight.size());
            in_flight.clear();
            return unconfirmed;
        }

        void reset() override {
            std::lock_guard<std::mutex> lock(link.mutex);
            in_flight.clear();
            for (auto &s: slots) s.used = 0;
            woken = false;
        }

    private:
        struct Slot : Transfer {
            std::vector<uint8_t> bytes;
        };

        FakeLink &link;
        const bool receive;
        std::vector<Slot> slots;
        std::deque<Transfer *> in_flight;
        bool woken = false;
    };

    void FakeLink::mute(const std::string &command) {
        std::lock_guard<std::mutex> lock(mutex);
        muted.insert(command);
    }

    void FakeLink::emit_line(const std::string &line) {
        std::lock_guard<std::mutex> lock(mutex);
        control_in.insert(control_in.end(), line.begin(), line.end());
        control_in.push_back('\n');
        changed.notify_all();
    }

    void FakeLink::feed_rx(const std::vector<uint8_t> &bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        rx.insert(rx.end(), bytes.begin(), bytes.end());
        changed.notify_all();
    }

    std::vector<uint8_t> FakeLink::rx_block(uint8_t flags, uint16_t seq, uint16_t valid, uint64_t time, size_t group,
                                            uint8_t seed) {
        std::vector<uint8_t> block(och::dsp::frame_block_bytes, 0);
        och::dsp::FrameHeader header;
        header.flags = flags;
        header.seq = seq;
        header.valid = valid;
        header.time = time;
        och::dsp::write_frame_header(block.data(), header);
        for (size_t i = 0; i < valid * group; ++i)
            block[och::dsp::frame_header_bytes + i] = static_cast<uint8_t>(seed + i);
        return block;
    }

    void FakeLink::unplug() {
        std::lock_guard<std::mutex> lock(mutex);
        unplugged = true;
        changed.notify_all();
    }

    void FakeLink::fail_next_submit() {
        std::lock_guard<std::mutex> lock(mutex);
        submit_fails = true;
    }

    std::vector<std::string> FakeLink::commands() const {
        std::lock_guard<std::mutex> lock(mutex);
        return log;
    }

    std::vector<uint8_t> FakeLink::tx_bytes() const {
        std::lock_guard<std::mutex> lock(mutex);
        return tx;
    }

    unsigned FakeLink::cancelled_transfers() const {
        std::lock_guard<std::mutex> lock(mutex);
        return cancelled;
    }

    unsigned FakeLink::transport_resets() const {
        std::lock_guard<std::mutex> lock(mutex);
        return resets;
    }

    size_t FakeLink::vendor_in(uint8_t request, uint16_t value, uint16_t, void *data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex);
        if (unplugged) throw Error("device unplugged");
        // CH32 bridge diagnostics, transport reset protocol 4. Anything else is rejected, like a
        // bridge without the firmware-update requests.
        if (request != 0xd0 || size < 8) throw Error("request rejected (STALL)");
        const uint32_t words[2] = {1396983601U, 4};
        memset(data, 0, size);
        memcpy(data, words, sizeof words);
        if (value == 2) {
            ++resets;
            rx.clear();
        }
        return size;
    }

    void FakeLink::vendor_out(uint8_t, uint16_t, uint16_t, const void *, size_t) {
        throw Error("request rejected (STALL)");
    }

    std::string FakeLink::sdk_reply(const std::string &command) {
        if (command == "host_protocol?") return "host_protocol=1";
        if (command == "system_info?") return system_info;
        if (command == "usb_status?") return "tx_main_fifo_groups=4096";
        if (command.rfind("rx_samp_freq=", 0) == 0) sample_rate = static_cast<uint32_t>(std::stoul(command.substr(13)));
        if (command == "clock_chain?") {
            const auto rate = std::to_string(sample_rate);
            const std::string chain = "983040000,245760000,122880000,61440000,61440000," + rate;
            return "ref=40000000 rx=" + chain + " tx=" + chain;
        }
        return {};
    }

    void FakeLink::accept_command(const std::string &line) {
        // "@<id> <command>"
        const auto space = line.find(' ');
        if (line.empty() || line[0] != '@' || space == std::string::npos) return;
        const std::string id = line.substr(1, space - 1), command = line.substr(space + 1);
        log.push_back(command);
        if (muted.count(command)) return;
        std::optional<std::string> text;
        if (on_command) text = on_command(command);
        if (!text) text = sdk_reply(command);
        std::string reply = "RSP id=" + id + " begin\n";
        if (!text->empty()) reply += *text + "\n";
        reply += "RSP id=" + id + " end result=0\n";
        control_in.insert(control_in.end(), reply.begin(), reply.end());
        changed.notify_all();
    }

    size_t FakeLink::transfer(Pipe pipe, uint8_t *data, size_t size, uint32_t) {
        std::lock_guard<std::mutex> lock(mutex);
        if (unplugged) throw Error(gone);
        if (pipe != Pipe::control_out) return 0; // nothing stale to drain, no diagnostics log
        control_out_partial.append(reinterpret_cast<const char *>(data), size);
        for (size_t end; (end = control_out_partial.find('\n')) != std::string::npos;) {
            accept_command(control_out_partial.substr(0, end));
            control_out_partial.erase(0, end + 1);
        }
        return size;
    }

    size_t FakeLink::read_control(uint8_t *data, size_t size) {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait_for(lock, std::chrono::milliseconds(20),
                         [&] { return unplugged || control_closed || !control_in.empty(); });
        if (control_closed) throw Error("submit: cancelled");
        if (unplugged) throw Error(gone);
        const size_t n = std::min(size, control_in.size());
        std::copy_n(control_in.begin(), n, data);
        control_in.erase(control_in.begin(), control_in.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }

    void FakeLink::shutdown_control() noexcept {
        std::lock_guard<std::mutex> lock(mutex);
        control_closed = true;
        changed.notify_all();
    }

    void FakeLink::reset_sample_pipes() {
        std::lock_guard<std::mutex> lock(mutex);
        if (unplugged) throw Error("EP1 reset failed: device unplugged");
    }

    std::unique_ptr<SampleQueue> FakeLink::open_samples(Pipe pipe, size_t transfer_bytes, unsigned depth) {
        return std::make_unique<Queue>(*this, pipe == Pipe::sample_in, transfer_bytes, depth);
    }
} // namespace och::transport
