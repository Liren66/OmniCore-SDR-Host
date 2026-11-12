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
    if (--it->second.references == 0) devices_.erase(it);
    update_lock();
}

void Registry::close_all() {
    devices_.clear();
    update_lock();
}

void Registry::shutdown() {
    devices_.clear();
    locked_ = false;
}

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
