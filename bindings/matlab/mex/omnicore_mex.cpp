// MEX gateway for OmniCoreSDR (MEX_PLAN.md). Built only on the public C API
// (include/omnicore/och.h) with the R2018a interleaved-complex MEX API.
//
//   omnicore_mex(command, args...)
//
// Internal interface of the +omnicore classes; it may change between versions. Commands:
//   v = omnicore_mex('version')                 struct: Library, Header, Abi
//   d = omnicore_mex('list')                    Nx1 struct array of connected devices
//   id = omnicore_mex('open', serial, request)  serial '' = the only device; request struct optional
//   omnicore_mex('close', id)                   drops one reference to the device
//   omnicore_mex('close_all')                   closes every device (tests, recovery)
//   n = omnicore_mex('count')                   open devices
//   s = omnicore_mex('info' | 'config' | 'status', id)
//   omnicore_mex('configure', id, request)      request: struct of Device.configure fields
//   v = omnicore_mex('get', id, name)           board settings (DeviceSession::get)
//   omnicore_mex('set', id, name, value)
#include "DeviceSession.hpp"
#include "Error.hpp"
#include "LibraryLoader.hpp"
#include "MxConvert.hpp"
#include "Registry.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mex.h>

using namespace omnicore_mex;

namespace {
    // Reports outlive run(): mexErrMsgIdAndTxt / mexWarnMsgIdAndTxt may longjmp, so they are called
    // only from mexFunction after every C++ object of the call has been destroyed.
    struct Report {
        char id[64];
        char message[1024];
    };
    constexpr int max_warnings = 16;
    Report error_report, warning_reports[max_warnings];
    int warning_count = 0;
    bool failed = false;

    void store(Report &r, const std::string &id, const std::string &message) {
        std::snprintf(r.id, sizeof r.id, "%s", id.c_str());
        std::snprintf(r.message, sizeof r.message, "%s", message.c_str());
    }

    void at_exit() {
        try {
            Registry::instance().shutdown();
        } catch (...) {}
    }

    void need_args(int nrhs, int count, const char *command) {
        if (nrhs < count) throw invalid_argument(std::string("omnicore_mex('") + command + "'): too few arguments");
    }

    // Field names of the request struct, as Device.configure accepts them.
    ConfigRequest to_request(const mxArray *s) {
        ConfigRequest r;
        if (!s || mxIsEmpty(s)) return r;
        if (!mxIsStruct(s) || mxGetNumberOfElements(s) != 1)
            throw invalid_argument("configuration must be a scalar struct");
        const int n = mxGetNumberOfFields(s);
        for (int i = 0; i < n; ++i) {
            const std::string name = mxGetFieldNameByNumber(s, i);
            const mxArray *v = mxGetFieldByNumber(s, 0, i);
            const char *what = name.c_str();
            if (name == "SampleRate")
                r.sample_rate = to_scalar(v, what);
            else if (name == "RxFrequency")
                r.rx_frequency = to_scalar(v, what);
            else if (name == "TxFrequency")
                r.tx_frequency = to_scalar(v, what);
            else if (name == "RxBandwidth")
                r.rx_bandwidth = to_scalar(v, what);
            else if (name == "TxBandwidth")
                r.tx_bandwidth = to_scalar(v, what);
            else if (name == "IQBits")
                r.iq_bits = to_scalar(v, what);
            else if (name == "RxGain")
                r.rx_gain = to_vector(v, what);
            else if (name == "TxGain")
                r.tx_gain = to_vector(v, what);
            else if (name == "RxChannels")
                r.rx_channels = to_vector(v, what);
            else if (name == "TxChannels")
                r.tx_channels = to_vector(v, what);
            else if (name == "RxGainMode")
                r.rx_gain_mode = to_string(v, what);
            else
                throw invalid_argument("unknown configuration field '" + name + "'");
        }
        return r;
    }

    mxArray *device_list() {
        size_t count = 0;
        check(och_list(nullptr, 0, &count), "list");
        std::vector<och_device_info> devices(count, och_struct<och_device_info>());
        if (count) check(och_list(devices.data(), devices.size(), &count), "list");
        devices.resize(std::min(count, devices.size()));
        std::sort(devices.begin(), devices.end(), [](const och_device_info &a, const och_device_info &b) {
            return std::strcmp(a.serial, b.serial) < 0;
        });
        const char *fields[] = {"Serial",       "Name",           "Firmware",      "UsbDriver",
                                "UsbSpeedMbps", "RxChannelCount", "TxChannelCount"};
        mxArray *out = mxCreateStructMatrix(devices.size(), 1, 7, fields);
        for (size_t i = 0; i < devices.size(); ++i) {
            const auto &d = devices[i];
            mxSetField(out, i, "Serial", make_string(d.serial));
            mxSetField(out, i, "Name", make_string(d.name));
            mxSetField(out, i, "Firmware", make_string(d.firmware));
            mxSetField(out, i, "UsbDriver", make_string(d.driver));
            mxSetField(out, i, "UsbSpeedMbps", make_double(d.usb_speed_mbps));
            mxSetField(out, i, "RxChannelCount", make_double(d.rx_channels));
            mxSetField(out, i, "TxChannelCount", make_double(d.tx_channels));
        }
        return out;
    }

    void dispatch(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[], Warnings &warnings) {
        (void) nlhs;
        need_args(nrhs, 1, "");
        const std::string command = to_string(prhs[0], "command");
        Registry &registry = Registry::instance();

        if (command == "version") {
            plhs[0] = StructBuilder()
                              .add("Library", make_string(och_version()))
                              .add("Header", make_string(OCH_VERSION))
                              .add("Abi", make_double(OCH_ABI_VERSION))
                              .build();
        } else if (command == "list") {
            plhs[0] = device_list();
        } else if (command == "open") {
            const std::string serial = nrhs > 1 ? to_string(prhs[1], "serial") : std::string();
            const ConfigRequest request = to_request(nrhs > 2 ? prhs[2] : nullptr);
            plhs[0] = make_id(registry.open(serial, request, warnings));
        } else if (command == "close") {
            need_args(nrhs, 2, "close");
            registry.close(to_id(prhs[1]));
        } else if (command == "close_all") {
            registry.close_all();
        } else if (command == "count") {
            plhs[0] = make_double(double(registry.size()));
        } else if (command == "info" || command == "config" || command == "status") {
            need_args(nrhs, 2, command.c_str());
            const DeviceSession &d = registry.device(to_id(prhs[1]));
            plhs[0] = command == "info" ? d.info_struct() : command == "config" ? d.config_struct() : d.status_struct();
        } else if (command == "configure") {
            need_args(nrhs, 3, "configure");
            registry.device(to_id(prhs[1])).configure(to_request(prhs[2]), warnings);
        } else if (command == "get") {
            need_args(nrhs, 3, "get");
            plhs[0] = registry.device(to_id(prhs[1])).get(to_string(prhs[2], "setting name"));
        } else if (command == "set") {
            need_args(nrhs, 4, "set");
            registry.device(to_id(prhs[1])).set(to_string(prhs[2], "setting name"), prhs[3]);
        } else {
            throw invalid_argument("unknown command '" + command + "'");
        }
    }

    // Everything C++ lives and dies inside this function.
    void run(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]) {
        failed = false;
        warning_count = 0;
        Warnings warnings;
        try {
            ensure_library_loaded();
            dispatch(nlhs, plhs, nrhs, prhs, warnings);
        } catch (const Error &e) {
            store(error_report, e.id(), e.what());
            failed = true;
        } catch (const std::bad_alloc &) {
            store(error_report, "omnicore:OutOfMemory", "out of memory");
            failed = true;
        } catch (const std::exception &e) {
            store(error_report, "omnicore:Internal", e.what());
            failed = true;
        } catch (...) {
            store(error_report, "omnicore:Internal", "unknown error");
            failed = true;
        }
        for (const auto &w: warnings)
            if (warning_count < max_warnings) store(warning_reports[warning_count++], w.id, w.message);
    }
} // namespace

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]) {
    static bool registered = false;
    if (!registered) {
        mexAtExit(at_exit);
        registered = true;
    }
    run(nlhs, plhs, nrhs, prhs);
    for (int i = 0; i < warning_count; ++i) mexWarnMsgIdAndTxt(warning_reports[i].id, "%s", warning_reports[i].message);
    if (failed) mexErrMsgIdAndTxt(error_report.id, "%s", error_report.message);
}
