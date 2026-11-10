// SPDX-License-Identifier: GPL-3.0-or-later
// CH32 bridge firmware: version query and USB update through the bridge
// bootloader. Device side and wire format: OmniCore-SDR-Firmware,
// OmniCore-SDR-Bridge/Common/ocfw.h (keep the constants below in sync).
#include "internal.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
using namespace och::core;
using namespace och::transport;

namespace {
    constexpr uint8_t REQ_ENTER_BOOT = 0xB0, REQ_INFO = 0xB1, REQ_ERASE = 0xB2, REQ_WRITE = 0xB3, REQ_COMMIT = 0xB4,
                      REQ_REBOOT = 0xB5;
    constexpr uint16_t KEY = 0x5AA5;
    constexpr uint32_t INFO_MAGIC = 0x4946434F, HEADER_MAGIC = 0x5746434F, HEADER_VERSION = 1, HW_ID = 0x48417B01;
    constexpr uint32_t STATE_BUSY = 1, STATE_DONE = 2;
    constexpr uint32_t PAGE = 256, MAX_CHUNK = 4096;
#pragma pack(push, 1)
    struct Info {
        uint32_t magic;
        uint16_t info_version;
        uint8_t mode, state;
        uint32_t running_version, boot_version, app_version, app_status, app_addr, app_max_size, chunk_max, error,
                progress, hw_id;
        char build_id[16];
    };
    struct Header {
        uint32_t magic, header_version, hw_id, image_size, image_crc32, fw_version, build_unix;
        char build_id[24];
        uint32_t reserved[2], header_crc32;
    };
#pragma pack(pop)
    static_assert(sizeof(Info) == 64 && sizeof(Header) == 64, "OCFW wire structs are 64 bytes");

    uint32_t crc32(const void *data, size_t n, uint32_t crc = 0) {
        auto p = static_cast<const uint8_t *>(data);
        crc = ~crc;
        while (n--) {
            crc ^= *p++;
            for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }
    std::string version_text(uint32_t v) {
        return std::to_string(v >> 16) + "." + std::to_string((v >> 8) & 255) + "." + std::to_string(v & 255);
    }
    const char *error_text(uint32_t e) {
        switch (e) {
        case 1:
            return "malformed request";
        case 2:
            return "address outside the application region";
        case 3:
            return "operations out of order";
        case 4:
            return "flash erase/program or read-back failed";
        case 5:
            return "image header rejected";
        case 6:
            return "image CRC mismatch after programming";
        default:
            return "unknown bootloader error";
        }
    }
    void pause(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

    // Legacy bridge firmware STALLs the request: false, not an error.
    bool read_info(Link &link, Info &i) {
        i = {};
        try {
            return link.vendor_in(REQ_INFO, 0, 0, &i, sizeof i) == sizeof i && i.magic == INFO_MAGIC;
        } catch (const Error &) {
            return false;
        }
    }
    void fill(och_firmware_info &out, const Info *i, const std::string &serial) {
        out = {};
        out.struct_size = sizeof out;
        copy_text(out.serial, sizeof out.serial, serial);
        if (!i) return;
        out.mode = i->mode;
        out.running_version = i->running_version;
        out.bootloader_version = i->boot_version;
        out.app_version = i->app_version;
        out.app_status = i->app_status;
        out.app_max_size = i->app_max_size;
        copy_text(out.build_id, sizeof out.build_id,
                  std::string(i->build_id, strnlen(i->build_id, sizeof i->build_id)));
    }

    struct Image {
        Header h{};
        std::vector<uint8_t> app;
    };
    Image load(const char *path) {
        require(path && *path, OCH_INVALID, "null firmware image path");
        std::ifstream f(std::filesystem::u8path(path), std::ios::binary); // path is UTF-8 on every platform
        require(f.good(), OCH_NOT_FOUND, std::string("cannot open firmware image ") + path);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        require(bytes.size() >= sizeof(Header) + PAGE, OCH_INVALID,
                "not an OmniCore bridge image (.ocfw): file too small");
        Image img;
        memcpy(&img.h, bytes.data(), sizeof img.h);
        require(img.h.magic == HEADER_MAGIC, OCH_INVALID,
                "not an OmniCore bridge image (.ocfw); Merge.bin is for WCH-Link only");
        require(img.h.header_version == HEADER_VERSION, OCH_UNSUPPORTED,
                "unsupported .ocfw header version " + std::to_string(img.h.header_version));
        require(img.h.header_crc32 == crc32(&img.h, offsetof(Header, header_crc32)), OCH_INVALID,
                "corrupt .ocfw header (CRC mismatch)");
        require(img.h.hw_id == HW_ID, OCH_INVALID, "image is for different hardware");
        require(img.h.image_size == bytes.size() - sizeof(Header), OCH_INVALID,
                "truncated .ocfw image (size mismatch)");
        img.app.assign(bytes.begin() + sizeof(Header), bytes.end());
        require(crc32(img.app.data(), img.app.size()) == img.h.image_crc32, OCH_INVALID,
                "corrupt .ocfw image (CRC mismatch)");
        return img;
    }

    std::vector<DeviceId> matching(const std::string &serial) {
        std::vector<DeviceId> out;
        for (auto &d: enumerate())
            if (serial.empty() || d.serial == serial) out.push_back(d);
        return out;
    }
    // One device for the whole operation, held like och_open holds it.
    struct Session {
        std::string serial;
        DeviceLock owner;
        std::unique_ptr<Link> link;
        Info info{};
        bool known = false;
        explicit Session(const char *wanted) {
            auto found = matching(wanted ? wanted : "");
            require(!found.empty(), OCH_NOT_FOUND, not_found_message());
            require(found.size() == 1, OCH_INVALID, "multiple devices; specify a serial number");
            serial = found.front().serial;
            const auto lock = owner.acquire(serial);
            require(lock != DeviceLock::Result::unavailable, OCH_IO, "device mutex unavailable");
            require(lock != DeviceLock::Result::busy, OCH_BUSY,
                    "device is open in an OmniCore Host process; close it first");
            link = och::transport::open(found.front());
            known = read_info(*link, info);
        }
        // After a reset: wait for the same serial to come back in the given mode.
        void reconnect(uint8_t mode, unsigned timeout_ms, const char *what) {
            link.reset();
            known = false;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            pause(300);
            while (std::chrono::steady_clock::now() < deadline) {
                auto found = matching(serial);
                if (found.size() == 1) {
                    try {
                        link = och::transport::open(found.front());
                        known = read_info(*link, info);
                        if (known && info.mode == mode) return;
                    } catch (const std::exception &) {}
                    link.reset();
                    known = false;
                }
                pause(200);
            }
            throw Failure(OCH_TIMEOUT, std::string("timed out waiting for the device to ") + what);
        }
        void send(uint8_t request, uint16_t value, uint16_t index, const void *data, uint16_t length,
                  const char *what) {
            try {
                link->vendor_out(request, value, index, data, length);
            } catch (const Error &) {
                throw Failure(OCH_IO, std::string("bootloader rejected ") + what);
            }
        }
        // Bootloader operations run outside its USB interrupt; poll until done.
        void finish(const char *what, unsigned timeout_ms) {
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            for (;;) {
                if (!read_info(*link, info)) {
                    size_t n = 0;
                    Info probe{};
                    std::string outcome = "ok";
                    try {
                        n = link->vendor_in(REQ_INFO, 0, 0, &probe, sizeof probe);
                    } catch (const Error &e) {
                        outcome = e.what();
                    }
                    throw Failure(OCH_IO, std::string("bootloader stopped answering during ") + what + " (control " +
                                                  outcome + ", " + std::to_string(n) + " bytes, state " +
                                                  std::to_string(info.state) + ")");
                }
                if (info.state != STATE_BUSY) break;
                require(std::chrono::steady_clock::now() < deadline, OCH_TIMEOUT,
                        std::string("bootloader timed out during ") + what);
                pause(1);
            }
            require(info.state == STATE_DONE, OCH_IO, std::string(what) + " failed: " + error_text(info.error));
        }
        void enter_bootloader() {
            require(known, OCH_UNSUPPORTED,
                    "bridge firmware has no USB bootloader; flash Merge.bin once with WCH-Link");
            if (info.mode == OCH_FW_MODE_BOOTLOADER) return;
            require(info.boot_version != 0, OCH_UNSUPPORTED,
                    "no bootloader installed; flash Merge.bin once with WCH-Link");
            send(REQ_ENTER_BOOT, KEY, 0, nullptr, 0, "bootloader entry");
            reconnect(OCH_FW_MODE_BOOTLOADER, 10000, "enter the bootloader");
        }
        void start_app() {
            require(known, OCH_UNSUPPORTED, "bridge firmware has no USB bootloader");
            if (info.mode == OCH_FW_MODE_APP) return;
            require(info.app_status & OCH_FW_APP_PROGRAMMED, OCH_UNSUPPORTED,
                    "no application installed; run och firmware update FILE");
            send(REQ_REBOOT, KEY, 0, nullptr, 0, "restart");
            // The application waits about 3 s before it enables USB.
            reconnect(OCH_FW_MODE_APP, 20000, "start the bridge application");
        }
    };
}

namespace och::core {
    bool firmware_probe(Link &link, och_firmware_info &out, const std::string &serial) {
        Info i;
        const bool ok = read_info(link, i);
        fill(out, ok ? &i : nullptr, serial);
        return ok;
    }
    och_firmware_image firmware_read_image(const char *path) {
        auto img = load(path);
        och_firmware_image out{};
        out.struct_size = sizeof out;
        out.version = img.h.fw_version;
        out.image_size = img.h.image_size;
        out.image_crc32 = img.h.image_crc32;
        out.build_unix = img.h.build_unix;
        copy_text(out.build_id, sizeof out.build_id,
                  std::string(img.h.build_id, strnlen(img.h.build_id, sizeof img.h.build_id)));
        return out;
    }
    och_firmware_info firmware_query(const char *serial) {
        Session s(serial);
        och_firmware_info out;
        fill(out, s.known ? &s.info : nullptr, s.serial);
        return out;
    }
    void firmware_enter_bootloader(const char *serial) {
        Session s(serial);
        s.enter_bootloader();
    }
    void firmware_start_app(const char *serial) {
        Session s(serial);
        s.start_app();
    }
    void firmware_update(const char *serial, const char *path, och_firmware_progress progress, void *user) {
        auto report = [&](uint32_t stage, uint32_t done, uint32_t total) {
            if (progress) progress(user, stage, done, total);
        };
        auto img = load(path);
        Session s(serial);
        report(OCH_FW_STAGE_BOOTLOADER, 0, 1);
        s.enter_bootloader();
        require(s.info.hw_id == img.h.hw_id, OCH_INVALID, "image is for different hardware");
        require(img.h.image_size <= s.info.app_max_size, OCH_INVALID, "image larger than the application region");
        report(OCH_FW_STAGE_BOOTLOADER, 1, 1);

        report(OCH_FW_STAGE_ERASE, 0, 1);
        s.send(REQ_ERASE, 0, 0, nullptr, 0, "erase");
        s.finish("erase", 15000);
        report(OCH_FW_STAGE_ERASE, 1, 1);

        const uint32_t chunk =
                std::min<uint32_t>(MAX_CHUNK, s.info.chunk_max ? s.info.chunk_max : MAX_CHUNK) & ~(PAGE - 1);
        std::vector<uint8_t> padded(img.app);
        padded.resize((padded.size() + PAGE - 1) / PAGE * PAGE, 0xFF);
        for (uint32_t offset = 0; offset < padded.size(); offset += chunk) {
            const uint32_t n = std::min<uint32_t>(chunk, static_cast<uint32_t>(padded.size()) - offset);
            s.send(REQ_WRITE, static_cast<uint16_t>(offset), static_cast<uint16_t>(offset >> 16),
                   padded.data() + offset, static_cast<uint16_t>(n), "write");
            s.finish("write", 5000);
            report(OCH_FW_STAGE_WRITE, std::min<uint32_t>(offset + n, img.h.image_size), img.h.image_size);
        }

        // The bootloader checks the CRC, then programs page 0 and the header.
        report(OCH_FW_STAGE_VERIFY, 0, 1);
        s.send(REQ_COMMIT, 0, 0, &img.h, sizeof img.h, "commit");
        s.finish("verify", 5000);
        require((s.info.app_status & OCH_FW_APP_IMAGE_OK) && s.info.app_version == img.h.fw_version, OCH_IO,
                "bootloader did not confirm the new image");
        report(OCH_FW_STAGE_VERIFY, 1, 1);

        report(OCH_FW_STAGE_RESTART, 0, 1);
        s.start_app();
        require(s.info.running_version == img.h.fw_version, OCH_PROTOCOL,
                "device restarted with " + version_text(s.info.running_version) + ", expected " +
                        version_text(img.h.fw_version));
        report(OCH_FW_STAGE_RESTART, 1, 1);
    }
}
