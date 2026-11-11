// Stream formats and setup. Data transfer (activate/read/write) comes in S2 (RX) and S3 (TX),
// see SOAPY_PLAN.md section 4.
#include "OmniCoreDevice.hpp"
#include <SoapySDR/Formats.hpp>

using namespace omnicore_soapy;

std::vector<std::string> OmniCoreDevice::getStreamFormats(int, size_t) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> formats{SOAPY_SDR_CS16, SOAPY_SDR_CF32};
    if (config_.usb_iq_bits == 8) formats.insert(formats.begin(), SOAPY_SDR_CS8);
    return formats;
}

// The 12-bit wire packing differs from SoapySDR's CS12 byte order, so CS16 is native: 12-bit
// samples left-aligned in 16 bits. In 8-bit wire mode CS8 is native.
std::string OmniCoreDevice::getNativeStreamFormat(int, size_t, double &fullScale) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (config_.usb_iq_bits == 8) {
        fullScale = 128;
        return SOAPY_SDR_CS8;
    }
    fullScale = 32768;
    return SOAPY_SDR_CS16;
}

SoapySDR::ArgInfoList OmniCoreDevice::getStreamArgsInfo(int, size_t) const {
    SoapySDR::ArgInfo transfer;
    transfer.key = "transfer_bytes";
    transfer.name = "USB transfer size";
    transfer.type = SoapySDR::ArgInfo::INT;
    transfer.units = "bytes";
    transfer.value = "196608";
    transfer.description = "multiple of 16384, at most 1048576";

    SoapySDR::ArgInfo depth;
    depth.key = "usb_queue_depth";
    depth.name = "USB queue depth";
    depth.type = SoapySDR::ArgInfo::INT;
    depth.value = "32";
    depth.description = "USB transfers kept in flight";

    SoapySDR::ArgInfo buffer;
    buffer.key = "host_buffer_bytes";
    buffer.name = "Host buffer";
    buffer.type = SoapySDR::ArgInfo::INT;
    buffer.units = "bytes";
    buffer.value = std::to_string(16u << 20);
    buffer.description = "host ring buffer, allocated at every activation";
    return {transfer, depth, buffer};
}

SoapySDR::Stream *OmniCoreDevice::setupStream(int, const std::string &, const std::vector<size_t> &,
                                              const SoapySDR::Kwargs &) {
    throw std::runtime_error("OmniCore: streaming is not implemented yet");
}
