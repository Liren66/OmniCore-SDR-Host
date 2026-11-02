// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "commands.hpp"
#include "presentation.hpp"
#include <locale>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

// Help and command-catalog views. Kept separate from main.cpp so the offline
// CLI tests can render them without a device or the Windows runtime.
namespace och_cli::help_view {
struct help_row { const char* usage; const char* text; };
struct help_group { const char* title; std::vector<help_row> rows; };
inline const std::vector<help_group>& help_groups() {
    static const std::vector<help_group> groups = {
        {"Device", {
            {"find", "List connected OmniCoreSDR devices"},
            {"info", "Identity, firmware, USB link, power snapshot"},
            {"power [status]", "USB-C power from the startup log"},
            {"rssi on|off", "Switch RX RSSI reporting (off after power-up)"},
            {"rssi [--count N] [--interval 0.5]", "RX1/RX2 RSSI, relative dB; --count 0 = until Ctrl+C"},
            {"version", "Host software version and C ABI"}}},
        {"Bridge firmware (CH32)", {
            {"firmware [info]", "Bridge version, bootloader, mode"},
            {"firmware check FILE.ocfw", "Validate an update image offline"},
            {"firmware update FILE.ocfw", "Install over USB (enters the bootloader)"},
            {"firmware boot", "Leave the bootloader, start the application"},
            {"firmware enter", "Restart into the bootloader (recovery)"}}},
        {"Control", {
            {"commands", "List the canonical control catalog"},
            {"get <name> [values]", "Query, e.g. och get rx.lo-frequency"},
            {"set <name> <values>", "Set, e.g. och set rx.lo-frequency 1G"},
            {"action <name> [values]", "Action, e.g. och action usb.clear"},
            {"config [RF options]", "Apply RF settings (no TX); bare: status"},
            {"selftest", "Digital loopback self-test (stops RF)"}}},
        {"FIR and clocks", {
            {"fir status", "Hardware FIR state and coefficients"},
            {"fir rx|tx|both on|off", "Enable or disable hardware FIR"},
            {"fir rx|tx load FILE", "Load FTR coefficients; that FIR stays off"},
            {"clock status", "Read the actual RX/TX clock chains"},
            {"clock sample-rate 30.72M", "Apply a shared sample-rate clock plan"},
            {"clock set --rx-chain CSV --tx-chain CSV", "Apply explicit six-stage clock plans"}}},
        {"Streaming", {
            {"rx --file PATH", "Capture IQ (+ PATH.metadata.jsonl)"},
            {"tx --tone 1M", "Transmit a generated complex tone"},
            {"tx --file PATH [--loop]", "Transmit an IQ file once, or repeat it"},
            {"bench --rx [--tx]", "Throughput benchmark, no file I/O"}}},
        {"Stream options", {
            {"--sample-rate 30.72M", "Per-channel rate for RX and TX"},
            {"--channels 1|2|1,2", "Physical channels (default 1,2)"},
            {"--rx-channels / --tx-channels", "Per direction (bench, config)"},
            {"--seconds 10", "Duration; 0 = until Ctrl+C"},
            {"--fail-fast", "bench: stop on repeated RX overflows"},
            {"--format FMT", "packed12 | sc16 (default) | fc32 | sc8"},
            {"--iq-bits 12|8", "USB wire precision (8: duplex, 1,2)"},
            {"--tone 1M --amplitude 0.25", "Generated tone, amplitude 0..1"},
            {"--overwrite", "Replace an existing RX capture"},
            {"--buffer-mib 64 --queue 32", "Host ring MiB, parallel USB requests"},
            {"--block 196608", "USB request size, multiple of 49152"}}},
        {"RF options", {
            {"--lo-frequency 1G", "RX+TX LO (or --rx-/--tx-lo-frequency)"},
            {"--bandwidth 18M", "Analog bandwidth (or --rx-bw / --tx-bw)"},
            {"--gain 30", "Manual RX gain, dB"},
            {"--agc manual|fast|slow|hybrid", "RX gain control mode"},
            {"--attenuation 10", "TX attenuation, dB"}}},
        {"Global options", {
            {"--serial SERIAL", "Select one of several devices"},
            {"--json", "JSON on stdout, events on stderr"},
            {"--no-color", "Plain text (or set NO_COLOR)"}}},
    };
    return groups;
}
inline const char* const help_notes[] = {
    "Numbers use Hz and dB; k/M/G suffixes are accepted. Channels identify physical ports.",
    "Legacy SDK names remain accepted by get/set using their original SDK units.",
    "The CLI opens the device exclusively. Typed FIR/clock/correction/reference/LO APIs coordinate active streams inside one process.",
    "FTR files with clock/bandwidth metadata require --coefficients-only to ignore it. A valid FIR file is not a frequency-response guarantee.",
    "bench uses the native wire format: packed12 by default, sc8 with --iq-bits 8. sc16 is left aligned; sc8 requires --iq-bits 8.",
    "USB2 needs an explicit lower --sample-rate. Run och info for measured references; the 30.72M streaming examples target USB3.",
    "RX strips every complete 16 KiB preamble; gaps and epochs go to the metadata sidecar. Existing captures need --overwrite.",
    "Finite-file EOF has no device end-of-burst acknowledgement: the tail can report TX underflow/starvation, and USB completion is not DAC completion.",
    "No timed transmission, hardware timestamps or PPS-scheduled streaming. PPS without 10M steers the 40M oscillator through the CH32 DAC; with 10M it is a frequency check only.",
    "Power data come from the HD3SS3220 startup log, not live current or LED readback; unknown supports_3a is null in JSON.",
};
inline void print_help(std::ostream& out) {
    presentation::title("help", "headless OmniCoreSDR control and IQ streaming", out);
    out << "\n  Usage: " << presentation::detail::paint(presentation::current(out), "1", "och <command> [options]")
        << "  [--serial SERIAL] [--json] [--no-color]\n";
    for (const auto& g : help_groups()) {
        presentation::group(g.title, out);
        for (const auto& r : g.rows) {
            std::string usage = r.usage;
            const size_t w = 30;
            if (usage.size() + 1 > w) {
                out << "    " << presentation::detail::paint(out, presentation::tone::accent, usage) << '\n';
                presentation::detail::lines(out, std::string(4 + w, ' '), 4 + w, r.text, presentation::tone::plain);
            } else {
                presentation::detail::lines(out, "    " + presentation::detail::paint(out, presentation::tone::accent, usage)
                    + std::string(w - usage.size(), ' '), 4 + w, r.text, presentation::tone::plain);
            }
        }
    }
    presentation::group("Notes", out);
    for (const auto* n : help_notes) presentation::note(n, out);
    presentation::group("Quick start", out);
    for (const auto* c : {"och find", "och info", "och get system.status",
                          "och bench --rx --tx --sample-rate 30.72M --channels 1,2 --seconds 10",
                          "och rx --file capture.sc16 --seconds 2"})
        out << "    " << presentation::detail::paint(out, presentation::tone::muted, "> ") << c << '\n';
    presentation::end_group(out);
}
inline std::string help_text() { std::ostringstream out; print_help(out); return out.str(); }

inline void print_catalog(std::ostream& out) {
    using presentation::tone;
    namespace d = presentation::detail;
    presentation::title("commands", "canonical control catalog", out);
    out << '\n';
    presentation::note("och get NAME  |  och set NAME VALUE  |  och action NAME", out);
    presentation::note("Frequencies in Hz (k/M/G accepted); gain and attenuation in dB. Use --json for the full machine-readable catalog.", out);
    std::string previous;
    for (const auto& e : commands::catalog()) {
        const auto name_group = e.name.substr(0, e.name.find('.'));
        if (name_group != previous) { presentation::group(name_group, out); previous = name_group; }
        std::string ops;
        if (!e.query.empty()) ops += "get";
        if (!e.setter.empty()) ops += std::string(ops.empty() ? "" : " ") + "set";
        if (!e.action.empty()) ops += std::string(ops.empty() ? "" : " ") + "action";
        std::string head = e.name;
        const size_t w = 26;
        std::string line = "    " + d::paint(out, e.supported ? tone::accent : tone::muted, head)
            + std::string(head.size() < w ? w - head.size() : 1, ' ') + d::paint(out, tone::plain, ops);
        if (!e.unit.empty()) line += d::paint(out, tone::muted, "  [" + e.unit + "]");
        if (!e.supported) line += "  " + d::paint(out, tone::warn, "reserved");
        out << line << '\n';
        d::lines(out, std::string(6, ' '), 6, e.summary, tone::muted);
        auto args = [&](const char* operation, const auto& parameters) {
            for (const auto& a : parameters) {
                std::ostringstream text; text.imbue(std::locale::classic());
                text << operation << ' ' << a.name << ": " << commands::format(a.minimum, 6) << ".."
                     << commands::format(a.maximum, 6) << ' ' << a.unit << ", step " << commands::format(a.step, 6);
                for (const auto& v : a.values) text << "  " << v.first << '=' << v.second;
                d::lines(out, std::string(6, ' '), 6, text.str(), tone::muted);
            }
        };
        args("get", e.query_args); args("set", e.set_args);
    }
    presentation::end_group(out);
}
} // namespace och_cli::help_view
