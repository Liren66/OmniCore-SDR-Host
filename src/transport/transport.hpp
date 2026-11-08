#pragma once
#ifdef _WIN32
#include "libusbk.hpp"
#else
#include "libusb.hpp"
#endif

namespace och::transport {
    // Linux usbfs can map DMA-capable buffers directly, avoiding a kernel copy
    // on every completion. Fall back to ordinary memory on unsupported kernels.
    class TransferBuffer {
        std::unique_ptr<uint8_t[]> heap;
        uint8_t *data = nullptr;
#ifndef _WIN32
        libusb_device_handle *device = nullptr;
        size_t size = 0;
#endif
    public:
        TransferBuffer(Usb &usb, size_t n) {
#ifndef _WIN32
            device = usb.device;
            size = n;
            if (device) data = libusb_dev_mem_alloc(device, n);
#else
            (void) usb;
#endif
            if (!data) {
                heap = std::make_unique<uint8_t[]>(n);
                data = heap.get();
            }
        }
        ~TransferBuffer() {
#ifndef _WIN32
            if (data && !heap) libusb_dev_mem_free(device, data, size);
#endif
        }
        TransferBuffer(const TransferBuffer &) = delete;
        TransferBuffer &operator=(const TransferBuffer &) = delete;
        uint8_t *get() const { return data; }
    };
}
