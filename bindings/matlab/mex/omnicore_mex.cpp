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
//   [x, meta] = omnicore_mex('receive', id, n, options)   one finite RX command (RxSession::receive)
//   sid = omnicore_mex('rx_create', id, options)           options: OutputDataType, BufferBytes
//   omnicore_mex('rx_start', sid, start_time)              start_time: seconds, or [] for now
//   omnicore_mex('rx_stop' | 'rx_destroy', sid)
//   [x, meta] = omnicore_mex('rx_read', sid, n, timeout)
//   tf = omnicore_mex('rx_running', sid)
//   t = omnicore_mex('time', id)                           struct: Time, TimeTicks, TickRate, Valid
//   s = omnicore_mex('transmit', id, x, options)           one burst; options: StartTime, Timeout, BufferBytes
//   sid = omnicore_mex('tx_create', id, options)           options: BufferBytes
//   omnicore_mex('tx_write', sid, x, options, timeout)     options: StartTime, EndBurst
//   omnicore_mex('tx_repeat', sid, x)                      transmit x repeatedly until tx_stop
//   omnicore_mex('tx_stop' | 'tx_destroy', sid)
//   s = omnicore_mex('tx_status', sid)                     TX events since the last call
//   s = omnicore_mex('tx_state', sid)                      struct: Active, Repeating
#include "DeviceSession.hpp"
#include "Error.hpp"
#include "LibraryLoader.hpp"
#include "MxConvert.hpp"
#include "Registry.hpp"
#include "RxSession.hpp"
#include <algorithm>
#include <cmath>
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

    // Fields shared by the RX / TX option structs that are not stream options.
    struct CallOptions {
        bool has_time = false;
        double start_time = 0; // seconds of hardware time
        double timeout = 1;
        bool end_burst = false;
    };

    // receive / rx_create / rx_start options. call == nullptr: only stream options are allowed.
    RxOptions to_rx_options(const mxArray *s, CallOptions *call) {
        RxOptions o;
        if (!s || mxIsEmpty(s)) return o;
        if (!mxIsStruct(s) || mxGetNumberOfElements(s) != 1) throw invalid_argument("options must be a scalar struct");
        const int n = mxGetNumberOfFields(s);
        for (int i = 0; i < n; ++i) {
            const std::string name = mxGetFieldNameByNumber(s, i);
            const mxArray *v = mxGetFieldByNumber(s, 0, i);
            if (name == "OutputDataType") {
                o.type = sample_type(to_string(v, "OutputDataType"));
            } else if (name == "BufferBytes") {
                const double b = to_scalar(v, "BufferBytes");
                if (b < 1 << 20 || b > 4294967295.0 || b != std::floor(b))
                    throw invalid_argument("BufferBytes must be a whole number from 1 MiB to 4 GiB");
                o.buffer_bytes = uint32_t(b);
            } else if (name == "Timeout" && call) {
                call->timeout = to_scalar(v, "Timeout");
            } else if (name == "StartTime" && call) {
                call->has_time = true;
                call->start_time = to_scalar(v, "StartTime");
            } else {
                throw invalid_argument("unknown RX option '" + name + "'");
            }
        }
        return o;
    }

    TxOptions to_tx_options(const mxArray *s, CallOptions *call) {
        TxOptions o;
        if (!s || mxIsEmpty(s)) return o;
        if (!mxIsStruct(s) || mxGetNumberOfElements(s) != 1) throw invalid_argument("options must be a scalar struct");
        const int n = mxGetNumberOfFields(s);
        for (int i = 0; i < n; ++i) {
            const std::string name = mxGetFieldNameByNumber(s, i);
            const mxArray *v = mxGetFieldByNumber(s, 0, i);
            if (name == "BufferBytes") {
                const double b = to_scalar(v, "BufferBytes");
                if (b < 1 << 20 || b > 4294967295.0 || b != std::floor(b))
                    throw invalid_argument("BufferBytes must be a whole number from 1 MiB to 4 GiB");
                o.buffer_bytes = uint32_t(b);
            } else if (name == "Timeout" && call) {
                call->timeout = to_scalar(v, "Timeout");
            } else if (name == "StartTime" && call) {
                call->has_time = true;
                call->start_time = to_scalar(v, "StartTime");
            } else if (name == "EndBurst" && call) {
                call->end_burst = to_bool(v, "EndBurst");
            } else {
                throw invalid_argument("unknown TX option '" + name + "'");
            }
        }
        return o;
    }

    TxWrite to_write(const DeviceSession &d, const CallOptions &c) {
        TxWrite w;
        w.has_time = c.has_time;
        if (c.has_time) w.time_ticks = d.to_ticks(c.start_time);
        w.end_burst = c.end_burst;
        return w;
    }

    size_t to_count(const mxArray *a, const char *what) {
        const double v = to_scalar(a, what);
        if (v < 0 || v != std::floor(v) || v > 9007199254740992.0)
            throw invalid_argument(std::string(what) + " must be a whole number of samples");
        return size_t(v);
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
        } else if (command == "receive") {
            need_args(nrhs, 3, "receive");
            DeviceSession &d = registry.device(to_id(prhs[1]));
            const size_t n = to_count(prhs[2], "N");
            CallOptions call;
            const RxOptions options = to_rx_options(nrhs > 3 ? prhs[3] : nullptr, &call);
            const uint64_t ticks = call.has_time ? d.to_ticks(call.start_time) : 0;
            RxSession::receive(d, options, n, call.timeout, call.has_time, ticks, &plhs[0],
                               nlhs > 1 ? &plhs[1] : nullptr);
        } else if (command == "rx_create") {
            need_args(nrhs, 2, "rx_create");
            const uint64_t device = to_id(prhs[1]);
            plhs[0] = make_id(registry.create_rx(device, to_rx_options(nrhs > 2 ? prhs[2] : nullptr, nullptr)));
        } else if (command == "rx_start") {
            need_args(nrhs, 2, "rx_start");
            RxSession &rx = registry.rx(to_id(prhs[1]));
            const bool has_time = nrhs > 2 && !mxIsEmpty(prhs[2]);
            rx.start(has_time, has_time ? rx.device().to_ticks(to_scalar(prhs[2], "StartTime")) : 0);
        } else if (command == "rx_stop") {
            need_args(nrhs, 2, "rx_stop");
            registry.rx(to_id(prhs[1])).stop();
        } else if (command == "rx_destroy" || command == "tx_destroy") {
            need_args(nrhs, 2, command.c_str());
            registry.destroy_stream(to_id(prhs[1]));
        } else if (command == "rx_running") {
            need_args(nrhs, 2, "rx_running");
            plhs[0] = make_bool(registry.rx(to_id(prhs[1])).running());
        } else if (command == "rx_read") {
            need_args(nrhs, 4, "rx_read");
            RxSession &rx = registry.rx(to_id(prhs[1]));
            rx.read(to_count(prhs[2], "N"), to_scalar(prhs[3], "Timeout"), &plhs[0], nlhs > 1 ? &plhs[1] : nullptr);
        } else if (command == "time") {
            need_args(nrhs, 2, "time");
            plhs[0] = registry.device(to_id(prhs[1])).time_struct();
        } else if (command == "transmit") {
            need_args(nrhs, 3, "transmit");
            DeviceSession &d = registry.device(to_id(prhs[1]));
            CallOptions call;
            const TxOptions options = to_tx_options(nrhs > 3 ? prhs[3] : nullptr, &call);
            if (call.end_burst) throw invalid_argument("unknown TX option 'EndBurst'");
            plhs[0] = TxSession::transmit(d, options, prhs[2], to_write(d, call), call.timeout, warnings);
        } else if (command == "tx_create") {
            need_args(nrhs, 2, "tx_create");
            const uint64_t device = to_id(prhs[1]);
            plhs[0] = make_id(registry.create_tx(device, to_tx_options(nrhs > 2 ? prhs[2] : nullptr, nullptr)));
        } else if (command == "tx_write") {
            need_args(nrhs, 5, "tx_write");
            TxSession &tx = registry.tx(to_id(prhs[1]));
            CallOptions call;
            to_tx_options(prhs[3], &call);
            tx.write(prhs[2], to_write(tx.device(), call), to_scalar(prhs[4], "Timeout"), warnings);
        } else if (command == "tx_repeat") {
            need_args(nrhs, 3, "tx_repeat");
            registry.tx(to_id(prhs[1])).repeat(prhs[2], warnings);
        } else if (command == "tx_stop") {
            need_args(nrhs, 2, "tx_stop");
            registry.tx(to_id(prhs[1])).stop(warnings);
        } else if (command == "tx_status") {
            need_args(nrhs, 2, "tx_status");
            plhs[0] = registry.tx(to_id(prhs[1])).status();
        } else if (command == "tx_state") {
            need_args(nrhs, 2, "tx_state");
            const TxSession &tx = registry.tx(to_id(prhs[1]));
            plhs[0] = StructBuilder().add("Active", make_bool(tx.active())).add("Repeating", make_bool(tx.repeating())).build();
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
