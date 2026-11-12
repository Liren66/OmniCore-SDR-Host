// Discovery and factory registration for driver=omnicore.
#include "OmniCoreDevice.hpp"
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Registry.hpp>
#include <algorithm>

using namespace omnicore_soapy;

namespace {
    // SoapySDR keys its device table on these results, and gr-soapy's source and sink share one
    // instance only when both resolve to the same entry. So the result for a device must not
    // change between calls or while it is open: serial and label only, sorted by serial.
    SoapySDR::KwargsList find_omnicore(const SoapySDR::Kwargs &args) {
        SoapySDR::KwargsList results;
        try {
            size_t count = 0;
            check(och_list(nullptr, 0, &count), "och_list");
            std::vector<och_device_info> devices(count, och_struct<och_device_info>());
            check(och_list(devices.data(), devices.size(), &count), "och_list");
            devices.resize(std::min(count, devices.size()));
            std::sort(devices.begin(), devices.end(), [](const och_device_info &a, const och_device_info &b) {
                return std::string(a.serial) < std::string(b.serial);
            });

            const auto wanted = args.find("serial");
            for (const auto &d: devices) {
                const std::string serial = d.serial;
                if (wanted != args.end() && !wanted->second.empty() && wanted->second != serial) continue;
                SoapySDR::Kwargs result;
                result["serial"] = serial;
                result["label"] = "OmniCore SDR " + serial;
                results.push_back(result);
            }
        } catch (const std::exception &e) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "OmniCore: device search failed: %s", e.what());
        }
        return results;
    }

    SoapySDR::Device *make_omnicore(const SoapySDR::Kwargs &args) { return new OmniCoreDevice(args); }

    const SoapySDR::Registry registration("omnicore", &find_omnicore, &make_omnicore, SOAPY_SDR_ABI_VERSION);
} // namespace
