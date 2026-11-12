// Handle table (MEX_PLAN.md 6.1, 6.2). MATLAB holds uint64 ids, never pointers: an unknown or
// stale id is an error, not a crash. Ids are never reused within a MATLAB session.
//
// One DeviceSession per serial: opening an already open device returns the same id and counts a
// reference, so several MATLAB objects (a Receiver and a Transmitter, M4) share one och_device
// instead of colliding on its exclusive lock. The session closes when the last reference goes.
//
// Streams (RxStream, TxStream) have ids of their own and belong to a device session: when the session
// closes, its streams are stopped and destroyed first (och_close refuses a device with streams).
//
// While any session exists the MEX file is locked (mexLock), so `clear mex` / `clear all` cannot
// unload it under live device handles; mexAtExit closes everything when MATLAB exits.
#pragma once
#include "DeviceSession.hpp"
#include "RxSession.hpp"
#include "TxSession.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace omnicore_mex {
    class Registry {
    public:
        static Registry &instance();

        // Returns the id of the session for `serial`, opening it if needed. On an already open
        // session the request is applied with och_configure.
        uint64_t open(const std::string &serial, const ConfigRequest &request, Warnings &warnings);
        // Drops one reference; closes the device when it was the last. Unknown ids are ignored, so
        // a MATLAB object deleted twice (or after close_all) is harmless.
        void close(uint64_t id);
        void close_all();
        // mexAtExit: closes everything without touching the MEX lock.
        void shutdown();

        DeviceSession &device(uint64_t id) const;
        size_t size() const { return devices_.size(); }

        uint64_t create_rx(uint64_t device_id, const RxOptions &options);
        RxSession &rx(uint64_t id) const;
        uint64_t create_tx(uint64_t device_id, const TxOptions &options);
        TxSession &tx(uint64_t id) const;
        // Unknown ids are ignored, like close.
        void destroy_stream(uint64_t id);
        size_t stream_count() const { return streams_.size(); }
        // Device notices for the warnings of this MEX call.
        void take_notices(Warnings &warnings);

    private:
        struct Entry {
            std::unique_ptr<DeviceSession> session;
            size_t references = 0;
        };
        struct StreamEntry {
            uint64_t device;
            std::unique_ptr<RxSession> rx;
            std::unique_ptr<TxSession> tx;
        };
        void update_lock();
        void erase_device(std::map<uint64_t, Entry>::iterator it);

        std::map<uint64_t, Entry> devices_;
        std::map<uint64_t, StreamEntry> streams_;
        uint64_t next_id_ = 1;
        bool locked_ = false;
    };
} // namespace omnicore_mex
