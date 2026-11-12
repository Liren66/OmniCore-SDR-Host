#include "Registry.hpp"
#include <vector>

using namespace omnicore_mex;

namespace {
    // och_open(NULL) picks the only connected device. Resolve that serial first, so a second
    // open without a serial finds the session already holding the device instead of hitting its
    // exclusive lock.
    std::string resolve_serial(const std::string &serial) {
        if (!serial.empty()) return serial;
        size_t count = 0;
        check(och_list(nullptr, 0, &count), "list");
        if (count != 1) return serial; // och_open reports none / more than one
        std::vector<och_device_info> devices(count, och_struct<och_device_info>());
        check(och_list(devices.data(), devices.size(), &count), "list");
        return count == 1 ? std::string(devices[0].serial) : serial;
    }
} // namespace

Registry &Registry::instance() {
    static Registry registry;
    return registry;
}

uint64_t Registry::open(const std::string &serial, const ConfigRequest &request, Warnings &warnings) {
    const std::string wanted = resolve_serial(serial);
    for (auto &d: devices_) {
        if (!wanted.empty() && d.second.session->serial() == wanted) {
            d.second.session->configure(request, warnings);
            ++d.second.references;
            return d.first;
        }
    }
    auto session = std::make_unique<DeviceSession>(wanted, request, warnings);
    const uint64_t id = next_id_++;
    devices_[id] = Entry{std::move(session), 1};
    update_lock();
    return id;
}

void Registry::close(uint64_t id) {
    const auto it = devices_.find(id);
    if (it == devices_.end()) return;
    if (--it->second.references == 0) erase_device(it);
    update_lock();
}

void Registry::erase_device(std::map<uint64_t, Entry>::iterator it) {
    for (auto s = streams_.begin(); s != streams_.end();) {
        if (s->second.device == it->first)
            s = streams_.erase(s);
        else
            ++s;
    }
    devices_.erase(it);
}

void Registry::close_all() {
    streams_.clear();
    devices_.clear();
    update_lock();
}

void Registry::shutdown() {
    streams_.clear();
    devices_.clear();
    locked_ = false;
}

uint64_t Registry::create_rx(uint64_t device_id, const RxOptions &options) {
    DeviceSession &d = device(device_id);
    const uint64_t id = next_id_++;
    streams_[id] = StreamEntry{device_id, std::make_unique<RxSession>(d, options)};
    return id;
}

RxSession &Registry::rx(uint64_t id) const {
    const auto it = streams_.find(id);
    if (it == streams_.end() || !it->second.rx)
        throw Error("omnicore:InvalidHandle", "the OmniCore RX stream was closed (its device was closed)");
    return *it->second.rx;
}

void Registry::destroy_stream(uint64_t id) { streams_.erase(id); }

DeviceSession &Registry::device(uint64_t id) const {
    const auto it = devices_.find(id);
    if (it == devices_.end())
        throw Error("omnicore:InvalidHandle", "the OmniCore device was closed (delete the object and open it again)");
    return *it->second.session;
}

void Registry::update_lock() {
    const bool want = !devices_.empty();
    if (want == locked_) return;
    if (want)
        mexLock();
    else
        mexUnlock();
    locked_ = want;
}
