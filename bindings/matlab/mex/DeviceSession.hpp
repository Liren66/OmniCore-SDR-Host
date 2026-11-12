// One open OmniCoreSDR (och_device) and its cached configuration (MEX_PLAN.md 3.1, 6.1), plus
// the device event thread (5.4): it polls och_poll_event and passes TX events to tx_events() for
// the TX session. The thread calls only och_* functions, never the MATLAB API.
#pragma once
#include "ConfigRequest.hpp"
#include "Error.hpp"
#include "TxEvents.hpp"
#include <atomic>
#include <mex.h>
#include <mutex>
#include <thread>
#include <omnicore/och.h>
#include <string>

namespace omnicore_mex {
    struct DeviceTime {
        uint64_t now = 0;       // sample ticks
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

        void configure(const ConfigRequest &request, Warnings &warnings);

        // Hardware sample time (och_get_time); tick_rate falls back to the sample rate.
        DeviceTime time() const;
        // Seconds of hardware time -> ticks (StartTime arguments).
        uint64_t to_ticks(double seconds) const;
        mxArray *time_struct() const;

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
        std::atomic<bool> stop_events_{false};
        std::thread events_;
    };
} // namespace omnicore_mex
