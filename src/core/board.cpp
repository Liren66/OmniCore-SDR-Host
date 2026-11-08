#include "internal.hpp"
#include <charconv>

using namespace och::core;

namespace {
    std::string field(const std::string &reply, const std::string &name) {
        size_t at = 0;
        while ((at = reply.find(name + "=", at)) != std::string::npos) {
            if (!at || reply[at - 1] == ' ' || reply[at - 1] == '\t' || reply[at - 1] == '\n' ||
                reply[at - 1] == '\r') {
                at += name.size() + 1;
                return reply.substr(at, reply.find_first_of(" \t\r\n", at) - at);
            }
            ++at;
        }
        throw Failure(OCH_PROTOCOL, "board reply missing " + name);
    }

    int64_t number(const std::string &text) {
        int64_t value = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        require(result.ec == std::errc() && result.ptr == text.data() + text.size(), OCH_PROTOCOL,
                "invalid board status integer");
        return value;
    }

    uint32_t bounded(const std::string &reply, const char *name, uint32_t maximum = UINT32_MAX) {
        const auto value = number(field(reply, name));
        require(value >= 0 && value <= maximum, OCH_PROTOCOL, std::string("board status out of range: ") + name);
        return static_cast<uint32_t>(value);
    }

    int32_t temperature(const std::string &reply, const char *name, uint32_t valid, uint32_t age) {
        const auto text = field(reply, name);
        if (!valid) {
            require(text == "unknown", OCH_PROTOCOL, "invalid temperature must remain unknown");
            return 0;
        }
        const auto value = number(text);
        require(value >= -40000 && value <= 125000 && age <= 2500, OCH_PROTOCOL, "invalid or stale Zynq temperature");
        return static_cast<int32_t>(value);
    }

    och_rgb_status read_rgb(och_device &device) {
        const auto reply = device.command("rgb_status?");
        och_rgb_status status{};
        status.struct_size = sizeof status;
        status.available = bounded(reply, "available", 1);
        status.enabled = bounded(reply, "enabled", 1);
        status.brightness_pct = bounded(reply, "brightness_pct", 100);
        const uint32_t channel_max = (status.brightness_pct * 255 + 50) / 100;
        status.red = bounded(reply, "red", channel_max);
        status.green = bounded(reply, "green", channel_max);
        status.blue = bounded(reply, "blue", channel_max);
        status.blink_hz = bounded(reply, "blink_hz", 4);
        const char *names[] = {"off", "initializing", "idle", "rx", "tx", "duplex", "recovering", "fault", "overtemp"};
        const auto state = field(reply, "state");
        status.state = 9;
        for (unsigned i = 0; i < 9; ++i) {
            if (state == names[i]) status.state = i;
        }
        require(status.available && (status.brightness_pct == 15 || status.brightness_pct == 25) && status.state < 9 &&
                        (status.state == OCH_RGB_OFF) == !status.enabled,
                OCH_PROTOCOL, "inconsistent RGB status");
        const unsigned blink = status.state == OCH_RGB_RECOVERING ? 1 : status.state == OCH_RGB_OVERTEMP ? 4 : 0;
        require(status.blink_hz == blink && (status.enabled || !(status.red || status.green || status.blue)),
                OCH_PROTOCOL, "inconsistent RGB output/blink state");
        return status;
    }

    och_fan_status read_fan(och_device &device) {
        const auto reply = device.command("fan_status?");
        och_fan_status status{};
        status.struct_size = sizeof status;
        status.available = bounded(reply, "available", 1);
        status.enabled = bounded(reply, "enabled", 1);
        status.frequency_hz = bounded(reply, "frequency_hz");
        status.duty_permille = bounded(reply, "duty_permille", 1000);
        status.target_permille = bounded(reply, "target_permille", 1000);
        status.boost = bounded(reply, "boost", 1);
        status.failsafe = bounded(reply, "failsafe", 1);
        status.temperature_valid = bounded(reply, "temp_valid", 1);
        status.temperature_age_ms = bounded(reply, "temp_age_ms");
        status.temperature_mc = temperature(reply, "temp_mC", status.temperature_valid, status.temperature_age_ms);
        status.overtemperature = bounded(reply, "overtemp", 1);
        require(status.available && status.frequency_hz == 25000 && field(reply, "source") == "zynq", OCH_PROTOCOL,
                "unsupported fan clock or temperature source");
        require(status.failsafe == (status.enabled && !status.temperature_valid), OCH_PROTOCOL,
                "inconsistent fan temperature failsafe");
        if (!status.enabled) {
            require(!status.duty_permille && !status.target_permille && !status.boost, OCH_PROTOCOL,
                    "disabled fan must request zero PWM");
        } else if (status.failsafe) {
            require(status.duty_permille == 1000 && status.target_permille == 1000 && !status.boost, OCH_PROTOCOL,
                    "fan temperature failure must request full PWM");
        } else {
            require(status.boost ? status.duty_permille == 1000 && status.target_permille > 0
                                 : status.duty_permille == status.target_permille,
                    OCH_PROTOCOL, "inconsistent fan target/boost duty");
        }
        return status;
    }
}

och_rgb_status och_device::get_rgb_status() {
    require_capability("board_rgb");
    return read_rgb(*this);
}

void och_device::set_rgb_enabled(unsigned enabled) {
    require(enabled <= 1, OCH_INVALID, "RGB enabled must be 0 or 1");
    if (get_rgb_status().enabled == enabled) return;
    command("rgb_enable=" + std::to_string(enabled));
    require(read_rgb(*this).enabled == enabled, OCH_PROTOCOL, "RGB enable readback mismatch");
}

och_fan_status och_device::get_fan_status() {
    require_capability("fan_control");
    return read_fan(*this);
}

void och_device::set_fan_enabled(unsigned enabled) {
    require(enabled <= 1, OCH_INVALID, "fan enabled must be 0 or 1");
    if (get_fan_status().enabled == enabled) return;
    // Board services never stop RF, reset transport or invalidate an RX lease.
    command("fan_enable=" + std::to_string(enabled));
    require(read_fan(*this).enabled == enabled, OCH_PROTOCOL, "fan enable readback mismatch");
}

och_core_temperature och_device::get_core_temperature() {
    require_capability("core_temp");
    const auto reply = command("core_temp?");
    och_core_temperature status{};
    status.struct_size = sizeof status;
    status.valid = bounded(reply, "valid", 1);
    status.age_ms = bounded(reply, "age_ms");
    status.overtemperature = bounded(reply, "overtemp", 1);
    status.milli_celsius = temperature(reply, "milli_celsius", status.valid, status.age_ms);
    require(field(reply, "source") == "zynq", OCH_PROTOCOL, "core temperature source must be Zynq");
    return status;
}
