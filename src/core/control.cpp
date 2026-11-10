// SDK command channel: "@id command" lines out, "RSP id=..." replies and "EVT ..." lines in.
#include "internal.hpp"
#include <charconv>
using namespace och::core;
using namespace och::transport;

ControlChannel::~ControlChannel() { stop(); }
void ControlChannel::start(Link &opened, std::function<void(const std::string &)> event,
                           std::function<void(const std::string &)> disconnect) {
    link = &opened;
    on_event = std::move(event);
    on_disconnect = std::move(disconnect);
    reader = std::thread([this] { loop(); });
}
void ControlChannel::stop() {
    stopping = true;
    if (link) link->shutdown_control();
    if (reader.joinable()) reader.join();
}
void ControlChannel::line(const std::string &s) {
    if (s.rfind("EVT ", 0) == 0) {
        on_event(s);
        return;
    }
    std::lock_guard<std::mutex> lock(reply_mutex);
    if (reply_done) return;
    if (s.rfind("RSP id=", 0) == 0) {
        auto p = s.data() + 7;
        uint32_t id = 0;
        auto parsed = std::from_chars(p, s.data() + s.size(), id);
        if (parsed.ec != std::errc() || !pending_id || id != pending_id) return;
        std::string tail(parsed.ptr, s.data() + s.size());
        if (tail == " begin") {
            reply_started = true;
            return;
        }
        if (tail.rfind(" end result=", 0) == 0) {
            if (!reply_started) {
                control_failed = true;
                control_error = "command end arrived without begin; reply integrity lost";
                reply_cv.notify_all();
                return;
            }
            int code = 0;
            auto rc = std::from_chars(tail.data() + 12, tail.data() + tail.size(), code);
            if (rc.ec != std::errc() || rc.ptr != tail.data() + tail.size()) {
                control_failed = true;
                control_error = "malformed command completion";
            }
            reply_result = code;
            reply_done = true;
            reply_cv.notify_all();
            return;
        }
    }
    if (pending_id && reply_started && !reply_done) {
        if (reply.size() + s.size() + 1 > 32768) {
            control_failed = true;
            control_error = "command reply exceeded 32 KiB";
            reply_cv.notify_all();
            return;
        }
        reply += s;
        reply += '\n';
    }
}
void ControlChannel::loop() {
    std::string partial;
    partial.reserve(4096);
    try {
        std::array<uint8_t, 4096> bytes{};
        while (!stopping) {
            size_t n = 0;
            try {
                n = link->read_control(bytes.data(), bytes.size());
            } catch (const Error &e) {
                throw Failure(OCH_IO, std::string("USB control ") + e.what());
            }
            for (size_t i = 0; i < n; ++i) {
                char c = static_cast<char>(bytes[i]);
                if (c == '\r') continue;
                if (c == '\n') {
                    if (!partial.empty()) line(partial);
                    partial.clear();
                } else {
                    if (partial.size() >= 32768) throw Failure(OCH_PROTOCOL, "unterminated control line");
                    partial += c;
                }
            }
        }
    } catch (const std::exception &e) {
        if (stopping) return;
        {
            std::lock_guard<std::mutex> lock(reply_mutex);
            control_failed = true;
            control_error = e.what();
        }
        reply_cv.notify_all();
        on_disconnect(e.what());
    }
}
std::string ControlChannel::command(const std::string &cmd, uint32_t timeout) {
    auto first = cmd.find_first_not_of(" \t");
    require(first != std::string::npos && cmd[first] != '@' &&
                    std::none_of(cmd.begin(), cmd.end(),
                                 [](unsigned char c) { return (c < 32 && c != '\t') || c >= 127; }),
            OCH_INVALID, "command must be one ASCII SDK command");
    std::lock_guard<std::mutex> serial(command_mutex);
    uint32_t id = ++next_id;
    if (!id) id = ++next_id;
    std::string framed = "@" + std::to_string(id) + " " + cmd + "\n";
    require(framed.size() <= 128, OCH_INVALID, "command exceeds SDK 127-character line limit");
    {
        std::lock_guard<std::mutex> lock(reply_mutex);
        require(!control_failed, OCH_IO, control_error);
        pending_id = id;
        reply.clear();
        reply_done = reply_started = false;
        reply_result = 0;
    }
    bool write_complete = false;
    try {
        auto n = bounded_transfer(*link, Pipe::control_out, reinterpret_cast<uint8_t *>(framed.data()), framed.size(),
                                  2000, "USB control");
        require(n == framed.size(), OCH_IO, "short USB command write");
        write_complete = true;
        std::unique_lock<std::mutex> lock(reply_mutex);
        bool done = reply_cv.wait_for(lock, std::chrono::milliseconds(timeout),
                                      [&] { return reply_done || control_failed; });
        if (!done) {
            control_failed = true;
            control_error = "SDK command timed out: " + cmd +
                            "; close/reopen device after firmware responds (envelope v1 required)";
            throw Failure(OCH_TIMEOUT, control_error);
        }
        require(!control_failed, OCH_IO, control_error);
        auto text = reply;
        auto rc = reply_result;
        pending_id = 0;
        require(!rc && text.find("ERR ") == std::string::npos && text.find("Error:") == std::string::npos &&
                        text.find("Invalid ") == std::string::npos,
                OCH_PROTOCOL, "SDK rejected " + cmd + ": " + text);
        return text;
    } catch (...) {
        std::lock_guard<std::mutex> lock(reply_mutex);
        pending_id = 0;
        if (!write_complete && !control_failed) {
            control_failed = true;
            control_error = "USB command write incomplete; close/reopen before sending another command";
        }
        throw;
    }
}
