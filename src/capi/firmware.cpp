// C entry points: CH32 bridge firmware query and update.
#include "common.hpp"
using namespace och::core;
using namespace och::capi;

extern "C" {
int och_firmware_query(const char *serial, och_firmware_info *p) {
    return protect([&] {
        out_struct(p);
        *p = firmware_query(serial);
        return OCH_OK;
    });
}

int och_get_firmware_info(och_device *d, och_firmware_info *p) {
    return protect([&] {
        device(d);
        out_struct(p);
        *p = d->firmware;
        return OCH_OK;
    });
}

int och_firmware_read_image(const char *path, och_firmware_image *p) {
    return protect([&] {
        out_struct(p);
        *p = firmware_read_image(path);
        return OCH_OK;
    });
}

int och_firmware_update(const char *serial, const char *path, och_firmware_progress progress, void *user) {
    return protect([&] {
        firmware_update(serial, path, progress, user);
        return OCH_OK;
    });
}

int och_firmware_enter_bootloader(const char *serial) {
    return protect([&] {
        firmware_enter_bootloader(serial);
        return OCH_OK;
    });
}

int och_firmware_start_app(const char *serial) {
    return protect([&] {
        firmware_start_app(serial);
        return OCH_OK;
    });
}
}
