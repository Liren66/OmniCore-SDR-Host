// One open OmniCoreSDR (och_device) and its cached configuration (MEX_PLAN.md 3.1, 6.1), plus
// the device event thread (5.4): it polls och_poll_event, passes TX events to tx_events() for
// the TX session and keeps the latest device events for Device.events (3.5). The thread calls
// only och_* functions, never the MATLAB API.
#pragma once
#include "ConfigRequest.hpp"
#include "Error.hpp"
#include "TxEvents.hpp"
#include <atomic>
#include <deque>
#include <mex.h>
#include <mutex>
#include <thread>
#include <omnicore/och.h>
#include <string>

namespace omnicore_mex {
    struct DeviceTime {
        uint64_t now = 0;       // sample ticks
        uint64_t last_pps = 0;  // time latched at the last PPS edge
        uint32_t tick_rate = 0; // ticks per second (= sample rate)
        bool valid = false;     // set since the last sample-rate change
    };

    class DeviceSession {
    public:
        // Opens the device (serial empty: the only connected one) and applies the default
        // configuration with `request` on top in one och_configure.
        DeviceSession(const std::string &serial, const ConfigRequest &request, Warnings &warnings);
        ~DeviceSession();
        DeviceSession(const DeviceSession &) = delete;
        DeviceSession &operator=(const DeviceSession &) = delete;

        const std::string &serial() const { return serial_; }
        och_device *handle() const { return dev_; } // streams (RxSession)
        const och_device_info &info() const { return info_; }
        och_config config() const;

        // Warns omnicore:TimeInvalidated when a sample-rate change resets a hardware time that was
        // set (valid).
        void configure(const ConfigRequest &request, Warnings &warnings);

        // Hardware sample time (och_get_time); tick_rate falls back to the sample rate.
        DeviceTime time() const;
        // Seconds of hardware time -> ticks (StartTime arguments).
        uint64_t to_ticks(double seconds) const;
        mxArray *time_struct() const;
        // och_set_time: now, or at the next PPS edge of the time source.
        void set_time(double seconds, bool at_next_pps);

        // Device events since the last call (up to the latest max_events), as a struct array.
        mxArray *events_struct();
        static constexpr size_t max_events = 256;
        // Transport errors since the last call, merged into one omnicore:Transport warning (5.4).
        void take_notices(Warnings &warnings);

        TxEvents &tx_events() { return tx_events_; }

        // MATLAB structs for info / config / status.
        mxArray *info_struct() const;
        mxArray *config_struct() const;
        mxArray *status_struct() const;

        // Board switches and sources that are not part of och_config (Device.m dependent
        // properties): RxLOSource, TxLOSource, PpsEnable, RssiEnable, RgbEnable, FanEnable,
        // RxDCCorrection, RxIQCorrection.
        mxArray *get(const std::string &name) const;
        void set(const std::string &name, const mxArray *value);

    private:
        void event_loop();

        och_device *dev_ = nullptr;
        och_device_info info_{};
        och_config config_{};
        std::string serial_;
        mutable std::mutex mutex_; // dev_ calls that change state, and config_
        TxEvents tx_events_;
        std::mutex events_mutex_;
        std::deque<och_event> events_; // latest max_events device events, for events_struct
        uint64_t transport_errors_ = 0;  // since take_notices, under events_mutex_
        std::string last_transport_error_;
        std::atomic<bool> stop_events_{false};
        std::thread event_thread_;
    };
} // namespace omnicore_mex
