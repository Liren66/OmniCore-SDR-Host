// SoapySDR device for OmniCoreSDR, built only on the public C API (include/omnicore/och.h).
// Design: SOAPY_PLAN.md.
#pragma once
#include <SoapySDR/Device.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <omnicore/och.h>
#include <string>
#include <thread>
#include <vector>

namespace omnicore_soapy {
    struct OmniCoreStream; // Streaming.cpp
    // Throws std::runtime_error with och_last_error() when rc is not OCH_OK.
    void check(int rc, const char *what);
    // Like check, but returns false for OCH_UNSUPPORTED (older firmware without the feature), so
    // GUIs that read every sensor and setting at startup do not fail on such devices.
    bool check_supported(int rc, const char *what);

    template<class T>
    T och_struct() {
        T s{};
        s.struct_size = sizeof s;
        return s;
    }

    class OmniCoreDevice final : public SoapySDR::Device {
    public:
        explicit OmniCoreDevice(const SoapySDR::Kwargs &args);
        ~OmniCoreDevice() override;

        // Identification
        std::string getDriverKey() const override;
        std::string getHardwareKey() const override;
        SoapySDR::Kwargs getHardwareInfo() const override;

        // Channels
        size_t getNumChannels(int direction) const override;
        bool getFullDuplex(int direction, size_t channel) const override;
        std::vector<std::string> listAntennas(int direction, size_t channel) const override;
        void setAntenna(int direction, size_t channel, const std::string &name) override;
        std::string getAntenna(int direction, size_t channel) const override;

        // Streams (Streaming.cpp). RX in S2; TX setup throws until S3.
        std::vector<std::string> getStreamFormats(int direction, size_t channel) const override;
        std::string getNativeStreamFormat(int direction, size_t channel, double &fullScale) const override;
        SoapySDR::ArgInfoList getStreamArgsInfo(int direction, size_t channel) const override;
        SoapySDR::Stream *setupStream(int direction, const std::string &format, const std::vector<size_t> &channels,
                                      const SoapySDR::Kwargs &args) override;
        void closeStream(SoapySDR::Stream *stream) override;
        size_t getStreamMTU(SoapySDR::Stream *stream) const override;
        int activateStream(SoapySDR::Stream *stream, int flags, long long timeNs, size_t numElems) override;
        int deactivateStream(SoapySDR::Stream *stream, int flags, long long timeNs) override;
        int readStream(SoapySDR::Stream *stream, void *const *buffs, size_t numElems, int &flags, long long &timeNs,
                       long timeoutUs) override;
        int writeStream(SoapySDR::Stream *stream, const void *const *buffs, size_t numElems, int &flags,
                        long long timeNs, long timeoutUs) override;
        int readStreamStatus(SoapySDR::Stream *stream, size_t &chanMask, int &flags, long long &timeNs,
                             long timeoutUs) override;

        // Front-end corrections
        bool hasDCOffsetMode(int direction, size_t channel) const override;
        void setDCOffsetMode(int direction, size_t channel, bool automatic) override;
        bool getDCOffsetMode(int direction, size_t channel) const override;
        bool hasIQBalanceMode(int direction, size_t channel) const override;
        void setIQBalanceMode(int direction, size_t channel, bool automatic) override;
        bool getIQBalanceMode(int direction, size_t channel) const override;

        // Gain
        std::vector<std::string> listGains(int direction, size_t channel) const override;
        bool hasGainMode(int direction, size_t channel) const override;
        void setGainMode(int direction, size_t channel, bool automatic) override;
        bool getGainMode(int direction, size_t channel) const override;
        void setGain(int direction, size_t channel, double value) override;
        void setGain(int direction, size_t channel, const std::string &name, double value) override;
        double getGain(int direction, size_t channel) const override;
        double getGain(int direction, size_t channel, const std::string &name) const override;
        SoapySDR::Range getGainRange(int direction, size_t channel) const override;
        SoapySDR::Range getGainRange(int direction, size_t channel, const std::string &name) const override;

        // Frequency
        void setFrequency(int direction, size_t channel, double frequency, const SoapySDR::Kwargs &args) override;
        void setFrequency(int direction, size_t channel, const std::string &name, double frequency,
                          const SoapySDR::Kwargs &args) override;
        double getFrequency(int direction, size_t channel) const override;
        double getFrequency(int direction, size_t channel, const std::string &name) const override;
        std::vector<std::string> listFrequencies(int direction, size_t channel) const override;
        SoapySDR::RangeList getFrequencyRange(int direction, size_t channel) const override;
        SoapySDR::RangeList getFrequencyRange(int direction, size_t channel, const std::string &name) const override;

        // Sample rate and bandwidth
        void setSampleRate(int direction, size_t channel, double rate) override;
        double getSampleRate(int direction, size_t channel) const override;
        SoapySDR::RangeList getSampleRateRange(int direction, size_t channel) const override;
        void setBandwidth(int direction, size_t channel, double bandwidth) override;
        double getBandwidth(int direction, size_t channel) const override;
        SoapySDR::RangeList getBandwidthRange(int direction, size_t channel) const override;

        // Clocking and time (Time.cpp)
        std::vector<std::string> listClockSources() const override;
        void setClockSource(const std::string &source) override;
        std::string getClockSource() const override;
        std::vector<std::string> listTimeSources() const override;
        void setTimeSource(const std::string &source) override;
        std::string getTimeSource() const override;
        bool hasHardwareTime(const std::string &what) const override;
        long long getHardwareTime(const std::string &what) const override;
        void setHardwareTime(long long timeNs, const std::string &what) override;

        // Sensors and settings (Sensors.cpp)
        std::vector<std::string> listSensors() const override;
        SoapySDR::ArgInfo getSensorInfo(const std::string &key) const override;
        std::string readSensor(const std::string &key) const override;
        std::vector<std::string> listSensors(int direction, size_t channel) const override;
        SoapySDR::ArgInfo getSensorInfo(int direction, size_t channel, const std::string &key) const override;
        std::string readSensor(int direction, size_t channel, const std::string &key) const override;
        SoapySDR::ArgInfoList getSettingInfo() const override;
        void writeSetting(const std::string &key, const std::string &value) override;
        std::string readSetting(const std::string &key) const override;

    private:
        // Applies a modified copy of the cached configuration; the cache changes only on success
        // and is then refreshed from och_get_config (applied sample rate, band-clamped gains).
        void reconfigure(const std::function<void(och_config &)> &change);
        void apply_locked(const std::function<void(och_config &)> &change); // caller holds mutex_
        void check_channel(int direction, size_t channel) const;
        double tick_rate() const;    // caller holds mutex_
        void stream_rate_changed();  // caller holds mutex_; after a sample-rate change

        // Device events (Events.cpp): one thread drains och_poll_event, which is a single
        // per-device queue, logs them and, while a TX stream is active, queues TX events for
        // readStreamStatus.
        void event_loop();
        void clear_tx_events();

        och_device *dev_ = nullptr;
        och_device_info info_{};
        och_config config_{};
        uint32_t auto_agc_mode_ = 2; // och rx_agc value used by setGainMode(true); default slow
        bool rx_correction_ = true;  // firmware supports RX DC/IQ tracking switches (checked at open)
        // Guards dev_ calls that change state, config_ and the stream slots. readStream does not
        // take it; lock order is mutex_ before a stream's io mutex.
        mutable std::mutex mutex_;
        OmniCoreStream *rx_stream_ = nullptr, *tx_stream_ = nullptr;
        std::atomic<bool> stop_events_{false};
        std::thread events_;

        std::atomic<bool> tx_listening_{false}; // a TX stream is active
        std::mutex tx_events_mutex_;
        std::condition_variable tx_events_cv_;
        std::deque<och_event> tx_events_; // bounded; oldest dropped
        size_t tx_events_dropped_ = 0;
    };
} // namespace omnicore_soapy
