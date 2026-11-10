// Transport interface used by src/core/. Standard library only: no platform or USB library
// types appear here. The interface grows as core call sites move onto it (TRANSPORT_PLAN.md).
#pragma once
#include <cstdint>

namespace och::transport {
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
