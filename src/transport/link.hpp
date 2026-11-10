// Transport interface used by src/core/. Standard library only: no platform or USB library
// types appear here. The interface grows as core call sites move onto it (TRANSPORT_PLAN.md).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace och::transport {
    // One connected device as found by enumerate(). Text is UTF-8.
    struct DeviceId {
        std::string path;   // backend-specific address; valid until the device is replugged
        std::string serial; // USB serial number, or a port-based name when the device has none
    };

    // Connected OmniCoreSDR devices this backend can open.
    std::vector<DeviceId> enumerate();

    // Backend name for och_device_info.driver.
    const char *driver_name();

    // Error text for "no device", with the platform's hint on what to check.
    const char *not_found_message();

    // Cross-process ownership of one device, by serial number. Released on destruction, and by
    // the operating system if the process dies.
    class DeviceLock {
    public:
        enum class Result {
            acquired,
            busy,        // another process holds it
            unavailable, // the lock itself could not be created
        };

        Result acquire(const std::string &serial);

    private:
        std::shared_ptr<void> held;
    };

    // The device's bulk pipes by role. Endpoint numbers stay inside the backends.
    enum class Pipe : uint8_t {
        sample_out,  // IQ to the device
        sample_in,   // IQ from the device
        control_out, // SDK command lines
        control_in,  // SDK replies and events
        diag_out,    // bridge queries (bridge firmware 1.0.1+)
        diag_in,     // bridge log and query replies
    };

    // One opened device. Each core Device owns one Link.
    class Link {
    public:
        virtual ~Link() = default;

        Link() = default;

        Link(const Link &) = delete;

        Link &operator=(const Link &) = delete;

        // Negotiated bus speed in Mbit/s: 5000 SuperSpeed, 480 High-Speed, 12 Full-Speed.
        virtual uint32_t speed_mbps() const = 0;

        // Whether the opened device has this pipe.
        virtual bool has(Pipe) const = 0;
    };
} // namespace och::transport
