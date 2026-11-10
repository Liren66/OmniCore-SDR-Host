// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef OMNICORE_OCH_HPP
#define OMNICORE_OCH_HPP
#include "och.h"
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace och {
    class error : public std::runtime_error {
    public:
        int code;

        explicit error(int value) : std::runtime_error(och_last_error()), code(value) {}
    };

    inline void check(int result) {
        if (result != OCH_OK) throw error(result);
    }

    inline och_config default_config() {
        och_config c{};
        och_default_config(&c);
        return c;
    }

    inline och_stream_args stream_args(och_direction direction) {
        och_stream_args a{};
        och_default_stream_args(&a, direction);
        return a;
    }

    inline std::vector<och_device_info> find() {
        size_t count = 0;
        check(och_list(nullptr, 0, &count));
        for (unsigned retry = 0; retry < 4; ++retry) {
            std::vector<och_device_info> list(count);
            for (auto &i: list) i.struct_size = sizeof(i);
            if (!count) return list;
            size_t found = 0;
            check(och_list(list.data(), list.size(), &found));
            if (found <= list.size()) {
                list.resize(found);
                return list;
            }
            count = found;
        }
        throw std::runtime_error("USB device list changed repeatedly; retry discovery");
    }

    // Declare device before its streams so reverse destruction releases streams first.
    // No C++ object, exception, or STL allocation crosses the DLL interface.
    class device {
        och_device *handle_ = nullptr;

    public:
        explicit device(const std::string &serial = {}) {
            check(och_open(serial.empty() ? nullptr : serial.c_str(), &handle_));
        }

        ~device() {
            if (handle_) (void) och_close(handle_);
        }

        device(const device &) = delete;

        device &operator=(const device &) = delete;

        device(device &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

        device &operator=(device &&) = delete;

        och_device *native_handle() const noexcept { return handle_; }

        void close() {
            if (handle_) {
                check(och_close(handle_));
                handle_ = nullptr;
            }
        }

        och_device_info info() const {
            och_device_info i{};
            i.struct_size = sizeof(i);
            check(och_get_info(handle_, &i));
            return i;
        }

        // Best-effort startup log snapshot, not live power or LED GPIO readback.
        // timeout 0 reads this handle's cache; missing logs remain unknown.
        och_power_status power_status(uint32_t timeout_ms = 500) const {
            och_power_status p{};
            p.struct_size = sizeof(p);
            p.source_current_ma = p.supports_3a = -1;
            check(och_get_power_status(handle_, &p, timeout_ms));
            return p;
        }

        // May be called by the management thread while recv/send are active.
        // The core preserves stream handles and resumes previously running paths;
        // see och_configure for discontinuity, TX padding and failure semantics.
        void configure(const och_config &c) { check(och_configure(handle_, &c)); }

        och_config config() const {
            och_config c{};
            c.struct_size = sizeof(c);
            check(och_get_config(handle_, &c));
            return c;
        }

        och_fir_config fir(och_direction direction) const {
            och_fir_config c{};
            c.struct_size = sizeof(c);
            check(och_get_fir(handle_, direction, &c));
            return c;
        }

        void set_fir(const och_fir_config &c) { check(och_set_fir(handle_, &c)); }

        void set_fir_enabled(bool rx_enabled, bool tx_enabled) {
            check(och_set_fir_enabled(handle_, rx_enabled ? 1u : 0u, tx_enabled ? 1u : 0u));
        }

        och_clock_config clock() const {
            och_clock_config c{};
            c.struct_size = sizeof(c);
            check(och_get_clock(handle_, &c));
            return c;
        }

        void set_clock(const och_clock_config &c) { check(och_set_clock(handle_, &c)); }
        void set_sample_rate(uint32_t sample_rate_hz) { check(och_set_sample_rate(handle_, sample_rate_hz)); }

        och_rx_correction_config rx_correction() const {
            och_rx_correction_config c{};
            c.struct_size = sizeof(c);
            check(och_get_rx_correction(handle_, &c));
            return c;
        }

        void set_rx_dc_correction(bool enabled) { check(och_set_rx_dc_correction(handle_, enabled ? 1u : 0u)); }
        void set_rx_iq_correction(bool enabled) { check(och_set_rx_iq_correction(handle_, enabled ? 1u : 0u)); }

        och_lo_source_config lo_source() const {
            och_lo_source_config c{};
            c.struct_size = sizeof(c);
            check(och_get_lo_source(handle_, &c));
            return c;
        }

        void set_rx_lo_source(och_signal_source source) { check(och_set_rx_lo_source(handle_, source)); }
        void set_tx_lo_source(och_signal_source source) { check(och_set_tx_lo_source(handle_, source)); }

        och_reference_status reference_status() const {
            och_reference_status c{};
            c.struct_size = sizeof(c);
            check(och_get_reference_status(handle_, &c));
            return c;
        }

        void set_reference_source(och_signal_source source) { check(och_set_reference_source(handle_, source)); }

        och_reference_test_result test_reference() {
            och_reference_test_result c{};
            c.struct_size = sizeof(c);
            check(och_test_reference(handle_, &c));
            return c;
        }

        och_pps_status pps_status() const {
            och_pps_status c{};
            c.struct_size = sizeof(c);
            check(och_get_pps_status(handle_, &c));
            return c;
        }

        void set_pps_enabled(bool enabled) { check(och_set_pps_enabled(handle_, enabled ? 1u : 0u)); }

        och_vcxo_status vcxo_status() const {
            och_vcxo_status c{};
            c.struct_size = sizeof(c);
            check(och_get_vcxo_status(handle_, &c));
            return c;
        }

        och_rgb_status rgb_status() const {
            och_rgb_status c{};
            c.struct_size = sizeof(c);
            check(och_get_rgb_status(handle_, &c));
            return c;
        }

        och_fan_status fan_status() const {
            och_fan_status c{};
            c.struct_size = sizeof(c);
            check(och_get_fan_status(handle_, &c));
            return c;
        }

        och_core_temperature core_temperature() const {
            och_core_temperature c{};
            c.struct_size = sizeof(c);
            check(och_get_core_temperature(handle_, &c));
            return c;
        }

        och_rx_rssi rx_rssi() const {
            och_rx_rssi c{};
            c.struct_size = sizeof(c);
            check(och_get_rx_rssi(handle_, &c));
            return c;
        }

        void set_rx_rssi_enabled(bool enabled) { check(och_set_rx_rssi_enabled(handle_, enabled ? 1u : 0u)); }
        void set_rgb_enabled(bool enabled) { check(och_set_rgb_enabled(handle_, enabled ? 1u : 0u)); }
        void set_fan_enabled(bool enabled) { check(och_set_fan_enabled(handle_, enabled ? 1u : 0u)); }

        std::string command(const std::string &command, uint32_t timeout_ms = 5000) {
            std::vector<char> text(16384);
            check(och_command(handle_, command.c_str(), text.data(), text.size(), timeout_ms));
            return text.data();
        }

        bool poll_event(och_event &event, uint32_t timeout_ms = 0) {
            event = {};
            event.struct_size = sizeof(event);
            int rc = och_poll_event(handle_, &event, timeout_ms);
            if (rc == OCH_TIMEOUT) return false;
            check(rc);
            return true;
        }
    };

    class stream {
        och_stream *handle_ = nullptr;

    public:
        stream(device &dev, const och_stream_args &args) {
            check(och_create_stream(dev.native_handle(), &args, &handle_));
        }

        ~stream() {
            if (handle_) {
                (void) och_stop(handle_, 2000);
                (void) och_destroy_stream(handle_);
            }
        }

        stream(const stream &) = delete;

        stream &operator=(const stream &) = delete;

        stream(stream &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

        stream &operator=(stream &&) = delete;

        och_stream *native_handle() const noexcept { return handle_; }
        void start() { check(och_start(handle_)); }
        void stop(uint32_t timeout_ms = 2000) { check(och_stop(handle_, timeout_ms)); }
        // USB drain only, not DAC/end-of-burst confirmation. Finite EOF may emit
        // tail underflow/starvation before stop; those events remain observable.
        void flush(uint32_t timeout_ms = 3000) { check(och_flush(handle_, timeout_ms)); }

        void close() {
            if (!handle_) return;
            auto old = std::exchange(handle_, nullptr);
            int rc = och_destroy_stream(old);
            // BUSY (outstanding RX lease) keeps ownership; other failures still
            // release the core handle and must never result in a second destroy.
            if (rc == OCH_BUSY) handle_ = old;
            check(rc);
        }

        och_stats stats() const {
            och_stats s{};
            s.struct_size = sizeof(s);
            check(och_get_stats(handle_, &s));
            return s;
        }

        // TIMEOUT (also a temporary reconfiguration pause) and STOPPED are returned
        // intact. Retry TIMEOUT; a control failure stops the session permanently.
        int recv(void *samples, size_t capacity, size_t &count, och_metadata &meta, uint32_t timeout_ms = 1000) {
            count = 0;
            meta = {};
            meta.struct_size = sizeof(meta);
            return och_recv(handle_, samples, capacity, &count, &meta, timeout_ms);
        }

        int send(const void *samples, size_t count, size_t &accepted, uint32_t timeout_ms = 1000) {
            accepted = 0;
            return och_send(handle_, samples, count, &accepted, timeout_ms);
        }
    };

    class rx_lease {
        och_stream *stream_ = nullptr;
        const void *data_ = nullptr;
        size_t size_ = 0;
        och_metadata metadata_{};

    public:
        rx_lease() = default;

        ~rx_lease() { release(); }

        rx_lease(const rx_lease &) = delete;

        rx_lease &operator=(const rx_lease &) = delete;

        int acquire(stream &stream, uint32_t timeout_ms = 1000) {
            if (stream_) return OCH_BUSY;
            metadata_ = {};
            metadata_.struct_size = sizeof(metadata_);
            int rc = och_rx_acquire(stream.native_handle(), &data_, &size_, &metadata_, timeout_ms);
            if (rc == OCH_OK) stream_ = stream.native_handle();
            return rc;
        }

        void release() noexcept {
            if (stream_) (void) och_rx_release(stream_);
            stream_ = nullptr;
            data_ = nullptr;
            size_ = 0;
        }

        const void *data() const noexcept { return data_; }
        size_t bytes() const noexcept { return size_; }
        const och_metadata &metadata() const noexcept { return metadata_; }
    };
} // namespace och
#endif
