// One open OmniCoreSDR (och_device) and its cached configuration (MEX_PLAN.md 3.1, 6.1).
#pragma once
#include "ConfigRequest.hpp"
#include "Error.hpp"
#include <mex.h>
#include <mutex>
#include <omnicore/och.h>
#include <string>

namespace omnicore_mex {
    class DeviceSession {
    public:
        // Opens the device (serial empty: the only connected one) and applies the default
        // configuration with `request` on top in one och_configure.
        DeviceSession(const std::string &serial, const ConfigRequest &request, Warnings &warnings);
        ~DeviceSession();
        DeviceSession(const DeviceSession &) = delete;
        DeviceSession &operator=(const DeviceSession &) = delete;

        const std::string &serial() const { return serial_; }
        const och_device_info &info() const { return info_; }
        och_config config() const;

        void configure(const ConfigRequest &request, Warnings &warnings);

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
        och_device *dev_ = nullptr;
        och_device_info info_{};
        och_config config_{};
        std::string serial_;
        mutable std::mutex mutex_; // dev_ calls that change state, and config_
    };
} // namespace omnicore_mex
