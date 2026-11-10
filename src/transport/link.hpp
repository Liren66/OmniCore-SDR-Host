// Transport interface used by src/core/. Standard library only: no platform or USB library
// types appear here. The interface grows as core call sites move onto it (TRANSPORT_PLAN.md).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace och::transport {
    // A transfer the backend could not complete. what() is the backend's own description.
    struct Error : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

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

    // One reusable transfer of a SampleQueue. The queue owns the memory; the stream worker fills
    // or reads data and keeps used up to date.
    struct Transfer {
        uint8_t *data = nullptr;
        size_t capacity = 0;
        size_t used = 0; // bytes to send, or the size of the read request
    };

    // Asynchronous transfers on one sample pipe. They complete in the order they were submitted.
    // Created with all its transfers and their memory, and reused across stream restarts.
    // Threads: wake() from any thread; reset() and the constructor from the control thread while
    // the worker is stopped; everything else from the stream worker.
    class SampleQueue {
    public:
        enum class Wait { completed, timeout, woken };

        virtual ~SampleQueue() = default;

        SampleQueue() = default;

        SampleQueue(const SampleQueue &) = delete;

        SampleQueue &operator=(const SampleQueue &) = delete;

        virtual size_t size() const = 0;

        virtual Transfer &slot(size_t index) = 0;

        // Largest single transfer the backend accepts on this pipe.
        virtual size_t max_transfer_bytes() = 0;

        // Prepares the pipe and the calling thread for streaming.
        virtual void start() = 0;

        // Queues slot.used bytes of one of this queue's transfers. Throws Error.
        virtual void submit(Transfer &) = 0;

        // Waits for the oldest submitted transfer; needs at least one in flight. completed: done
        // is that transfer and bytes its length, and it may be submitted again. woken: wake() was
        // called. Throws Error if the wait or the transfer failed; the transfer stays in flight.
        virtual Wait wait(uint32_t timeout_ms, Transfer *&done, size_t &bytes) = 0;

        // Makes wait() return woken, now and on every later call until reset().
        virtual void wake() noexcept = 0;

        // Cancels the transfers in flight and waits until the backend has finished with all of
        // them, then leaves streaming mode. Returns the sum of used over the cancelled transfers.
        virtual uint64_t stop() = 0;

        // Back to the state after construction: nothing in flight, used 0, not woken.
        virtual void reset() = 0;
    };

    class Link;

    // Opens a device found by enumerate(). Throws std::runtime_error with the backend's reason.
    std::unique_ptr<Link> open(const DeviceId &);

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

        // Vendor request to the device on the default control pipe. vendor_in returns the bytes
        // received (at most size). Both throw Error, also when the device rejects (STALLs) the
        // request. size is at most 65535.
        virtual size_t vendor_in(uint8_t request, uint16_t value, uint16_t index, void *data, size_t size) = 0;

        virtual void vendor_out(uint8_t request, uint16_t value, uint16_t index, const void *data, size_t size) = 0;

        // One bounded transfer on a bulk pipe, in the pipe's direction. Returns the bytes
        // transferred; 0 when timeout_ms (at least 1) passes with none. Throws Error, whose text
        // starts with "submit: " or "completion: ". Different pipes may be used concurrently;
        // the caller serializes transfers on one pipe.
        virtual size_t transfer(Pipe, uint8_t *data, size_t size, uint32_t timeout_ms) = 0;

        // Reads Pipe::control_in for the control thread. Returns the bytes received, or 0 when
        // this wait ended without data: the backend chooses between short timed reads and one
        // request that stays pending. Throws Error like transfer(), also after shutdown_control().
        virtual size_t read_control(uint8_t *data, size_t size) = 0;

        // Ends a pending read_control() and refuses further ones. Any thread; cannot be undone.
        virtual void shutdown_control() noexcept = 0;

        // Host side of the cleanup between streams, after the device has cleared its buffers:
        // returns both sample pipes to a clean state in which Pipe::sample_in can be drained
        // with transfer(). Only while no sample transfer is in flight. Throws Error.
        virtual void reset_sample_pipes() = 0;

        // Queue of depth transfers of transfer_bytes each on Pipe::sample_in or sample_out.
        // The Link must outlive it. Throws Error.
        virtual std::unique_ptr<SampleQueue> open_samples(Pipe, size_t transfer_bytes, unsigned depth) = 0;
    };
} // namespace och::transport
