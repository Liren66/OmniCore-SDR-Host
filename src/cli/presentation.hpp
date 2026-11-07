// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "usb2_reference.hpp"
#include "stream_assessment.hpp"
#include <omnicore/och.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Human-readable terminal output. No hardware access.
//
// Styling is opt-in per stream: by default every stream (including string
// streams used by tests and --json help) receives plain 7-bit ASCII. main()
// enables ANSI colour and Unicode rules for std::cout / std::cerr only when
// that handle is an interactive console with VT processing available, and
// NO_COLOR / --no-color are not set. Redirected output therefore stays
// grep-friendly and byte-stable.
namespace och_cli::presentation {

struct style {
    bool color = false;   // ANSI SGR sequences
    bool unicode = false; // box-drawing rules, bullets and progress blocks
};
enum class tone { plain, accent, good, warn, bad, muted };
enum class level { ok, info, warn, fail, run, stop, prep, note };

namespace detail {
constexpr size_t width = 78;       // total line budget
constexpr size_t label_width = 28; // label column, measured from the left margin

struct stream_state { style look; size_t indent = 2; };
inline stream_state& out_state() { static stream_state s; return s; }
inline stream_state& err_state() { static stream_state s; return s; }
inline stream_state& state(const std::ostream& out) {
    if (&out == &std::cout) return out_state();
    if (&out == &std::cerr || &out == &std::clog) return err_state();
    // Other streams (string streams in tests, --json help) are never styled;
    // only their indentation is tracked.
    static thread_local std::map<const std::ostream*, stream_state> others;
    auto& entry = others[&out];
    entry.look = style{};
    return entry;
}
inline const style& look(const std::ostream& out) { return state(out).look; }

// --- text helpers -----------------------------------------------------------
// Keep arbitrary SDK/identity bytes visible without terminal escape sequences.
// Non-ASCII bytes use reversible \xHH escapes, independent of console codepage.
inline std::string ascii(const std::string& input) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : input) {
        if (c >= 32 && c <= 126) out += static_cast<char>(c);
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else { out += "\\x"; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
template<size_t N> inline std::string text(const char (&input)[N]) {
    const auto end = std::find(input, input + N, '\0');
    return std::string(input, end);
}
inline std::string trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}
inline std::string fixed(double value, unsigned digits = 3) {
    if (!std::isfinite(value)) return "Unknown";
    std::ostringstream out; out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(digits) << value;
    return out.str();
}
inline std::string hex(uint32_t value, unsigned digits = 2) {
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(digits) << value;
    return out.str();
}
// 1234567 -> "1,234,567": long counters are read at a glance.
inline std::string grouped(uint64_t value) {
    auto digits = std::to_string(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<size_t>(i), ",");
    return digits;
}
inline std::string bytes(uint64_t value) {
    const double v = static_cast<double>(value);
    if (value >= 1073741824ull) return fixed(v / 1073741824.0, 2) + " GiB";
    if (value >= 1048576ull) return fixed(v / 1048576.0, 2) + " MiB";
    if (value >= 1024ull) return fixed(v / 1024.0, 1) + " KiB";
    return std::to_string(value) + " B";
}
inline std::string frequency(uint64_t hz) {
    if (!hz) return "Unknown";
    const std::string scaled = hz >= 1000000000ull ? fixed(static_cast<double>(hz) / 1e9, 6) + " GHz" :
        hz >= 1000000 ? fixed(static_cast<double>(hz) / 1e6, 6) + " MHz" :
        hz >= 1000 ? fixed(static_cast<double>(hz) / 1e3, 3) + " kHz" : std::to_string(hz) + " Hz";
    return hz >= 1000 ? scaled + "  (" + std::to_string(hz) + " Hz)" : scaled;
}
inline std::string rate(uint64_t hz) {
    return hz ? fixed(static_cast<double>(hz) / 1e6, 3) + " MSPS per channel" : "Unknown";
}
inline std::string mask(uint32_t value) {
    const char* selected = value == 0 ? "Disabled" : value == 1 ? "CH1" :
        value == 2 ? "CH2" : value == 3 ? "CH1 + CH2" : "Unknown channel mask";
    return std::string(selected) + "  (mask " + std::to_string(value) + ")";
}
inline const char* direction(uint32_t value) {
    return value == OCH_RX ? "RX" : value == OCH_TX ? "TX" : "Unknown direction";
}
inline std::string binary(uint32_t value) {
    return value == 0 ? "No" : value == 1 ? "Yes" : "Unknown (" + std::to_string(value) + ")";
}
inline tone binary_tone(uint32_t value, bool yes_is_good = true) {
    if (value > 1) return tone::warn;
    return (value == 1) == yes_is_good ? tone::good : tone::muted;
}
inline std::string nonempty(const std::string& value, const char* fallback = "Unknown") {
    return value.empty() ? fallback : value;
}
inline std::string lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return value;
}
inline std::string upper(std::string value) {
    for (auto& c : value) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
    return value;
}
// Display columns of UTF-8 text. All glyphs used here are single width.
inline size_t columns(const std::string& s) {
    size_t n = 0; bool escape = false;
    for (unsigned char c : s) {
        if (escape) { if (c == 'm') escape = false; continue; }
        if (c == 0x1B) { escape = true; continue; }
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}
inline std::string repeat(const char* glyph, size_t count) {
    std::string out; for (size_t i = 0; i < count; ++i) out += glyph; return out;
}

// --- colour -----------------------------------------------------------------
inline const char* sgr(tone t) {
    switch (t) {
    case tone::accent: return "1";
    case tone::good: return "32";
    case tone::warn: return "33";
    case tone::bad: return "1;31";
    case tone::muted: return "90";
    default: return "";
    }
}
inline std::string paint(const style& st, const char* code, const std::string& value) {
    if (!st.color || !code || !*code || value.empty()) return value;
    return "\x1b[" + std::string(code) + "m" + value + "\x1b[0m";
}
inline std::string paint(const std::ostream& out, tone t, const std::string& value) {
    return paint(look(out), sgr(t), value);
}

// Word-wrap plain ASCII text into chunks no wider than room. Separators are
// kept with the preceding chunk so no source text is silently removed.
inline std::vector<std::string> chunks(const std::string& value, size_t room) {
    std::vector<std::string> out;
    if (room < 8) room = 8;
    size_t offset = 0;
    do {
        size_t count = std::min(room, value.size() - offset);
        if (offset + count < value.size()) {
            const auto space = value.rfind(' ', offset + count - 1);
            if (space != std::string::npos && space > offset) count = space - offset + 1;
        }
        auto part = value.substr(offset, count);
        while (part.size() > 1 && part.back() == ' ') part.pop_back();
        out.push_back(part);
        offset += count;
    } while (offset < value.size());
    return out;
}
// Emit value after a prefix; continuation lines are aligned under the value.
inline void lines(std::ostream& out, const std::string& first_prefix, size_t prefix_columns,
                  const std::string& value, tone t) {
    const auto parts = chunks(value, width > prefix_columns ? width - prefix_columns : 8);
    const std::string continuation(prefix_columns, ' ');
    for (size_t i = 0; i < parts.size(); ++i)
        out << (i ? continuation : first_prefix) << paint(out, t, parts[i]) << '\n';
}

inline std::string badge(const std::ostream& out, level l) {
    struct spec { const char* word; const char* colour; };
    static const spec specs[] = {
        // Regular weight: bold black renders as grey on several terminals.
        {"OK", "30;42"}, {"INFO", "30;46"}, {"WARN", "30;43"}, {"FAIL", "97;41"},
        {"RUN", "97;44"}, {"STOP", "30;47"}, {"PREP", "30;47"}, {"NOTE", "30;47"}};
    const auto& s = specs[static_cast<int>(l)];
    std::string word = s.word;
    const auto& st = look(out);
    if (!st.color) {
        if (word.size() == 2) word = " " + word + " ";
        while (word.size() < 4) word += ' ';
        return "[" + word + "]";
    }
    const size_t pad = 6 - word.size();
    return paint(st, s.colour, std::string(pad / 2, ' ') + word + std::string(pad - pad / 2, ' '));
}

// --- key=value parsing for SDK replies ---------------------------------------
inline bool status_group(const std::string& name) {
    const auto key = lower(name);
    return key == "rgb_status" || key == "fan_status" || key == "core_temp" || key == "rx_rssi" ||
           key == "board_capabilities" || key == "system_status" || key == "temperature" || key == "rssi" ||
           key == "rf" || key == "usb_stream" || key == "faults" || key == "rx_resync" ||
           key == "usb_status" || key == "errors" || key == "rx_ready";
}
inline bool key_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
}
struct key_position { size_t start, equals; };
// Find only top-level key=value starts. Quotes/braces inside values remain
// intact. Unknown text is always printed rather than silently discarded.
inline std::vector<key_position> keys(const std::string& line) {
    std::vector<key_position> result;
    char quote = 0; bool escaped = false; int depth = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (quote) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == quote) quote = 0;
            continue;
        }
        if (ch == '\'' || ch == '"') { quote = ch; continue; }
        if (ch == '{' || ch == '[' || ch == '(') { ++depth; continue; }
        if (ch == '}' || ch == ']' || ch == ')') { if (depth > 0) --depth; continue; }
        if (depth || (i && line[i - 1] != ' ' && line[i - 1] != '\t') || !key_char(ch)) continue;
        size_t end = i;
        while (end < line.size() && key_char(line[end])) ++end;
        if (end < line.size() && line[end] == '=') { result.push_back({i, end}); i = end; }
    }
    return result;
}
// Counters whose non-zero value deserves attention in a status dump.
inline tone value_tone(const std::string& key, const std::string& value) {
    const auto k = lower(key);
    const bool fault_name = k.find("error") != std::string::npos || k.find("fail") != std::string::npos ||
        k.find("overflow") != std::string::npos || k.find("underflow") != std::string::npos ||
        k.find("drop") != std::string::npos || k == "overtemp" || k.find("timeout") != std::string::npos ||
        k.find("protocol") != std::string::npos;
    if (value == "unknown" || value == "(empty)") return tone::muted;
    if (!fault_name) return tone::plain;
    bool all_zero = !value.empty();
    for (char c : value) if (c != '0' && c != 'x' && c != 'X' && c != ',' && c != ' ' && c != '/') { all_zero = false; break; }
    if (value == "-1") return tone::muted;
    return all_zero ? tone::plain : tone::warn;
}
} // namespace detail

// --- configuration ------------------------------------------------------------
inline void configure(std::ostream& out, const style& st) { detail::state(out).look = st; }
inline const style& current(const std::ostream& out) { return detail::look(out); }

// --- primitives ---------------------------------------------------------------
// Level 1: a full-width heading. Resets indentation for following fields.
inline void section(const std::string& title, std::ostream& out) {
    auto& s = detail::state(out); s.indent = 2;
    const auto text = detail::upper(detail::ascii(title));
    const bool u = s.look.unicode;
    const char* heavy = u ? "\xE2\x94\x81" : "="; // ━
    out << '\n';
    if (text.size() + 8 > detail::width) {
        out << detail::paint(out, tone::muted, detail::repeat(heavy, detail::width)) << '\n';
        detail::lines(out, " ", 1, text, tone::accent);
        return;
    }
    const size_t tail = detail::width - text.size() - 4;
    out << detail::paint(s.look, "36", detail::repeat(heavy, 2)) << ' '
        << detail::paint(s.look, "1;96", text) << ' '
        << detail::paint(s.look, "36", detail::repeat(heavy, tail)) << '\n';
}
// Level 2: a named group inside a section; following fields are indented.
inline void group(const std::string& title, std::ostream& out) {
    auto& s = detail::state(out);
    const auto text = detail::ascii(title);
    const bool u = s.look.unicode;
    const char* light = u ? "\xE2\x94\x80" : "-"; // ─
    const size_t used = 2 + text.size() + 1;
    out << '\n' << "  " << detail::paint(s.look, "1;97", text) << ' '
        << detail::paint(s.look, "90", detail::repeat(light, used + 4 < detail::width ? detail::width - used : 4))
        << '\n';
    s.indent = 4;
}
inline void end_group(std::ostream& out) { detail::state(out).indent = 2; }

inline void field_text(const std::string& label, const std::string& value, std::ostream& out, tone t = tone::plain) {
    const auto& s = detail::state(out);
    const auto name = detail::ascii(label);
    const std::string margin(s.indent, ' ');
    const size_t column = detail::label_width;
    const auto rendered = detail::ascii(value);
    if (s.indent + name.size() + 1 > column) {
        out << margin << name << '\n';
        detail::lines(out, std::string(column + 2, ' '), column + 2, rendered, t);
        return;
    }
    const std::string prefix = margin + name + std::string(column - s.indent - name.size(), ' ') + "  ";
    detail::lines(out, prefix, prefix.size(), rendered, t);
}
template<class T> inline void field(const std::string& label, const T& value, std::ostream& out, tone t = tone::plain) {
    std::ostringstream rendered; rendered.imbue(std::locale::classic()); rendered << value;
    field_text(label, rendered.str(), out, t);
}
// Footnote-style remark: dim, prefixed with a bullet.
inline void note(const std::string& value, std::ostream& out) {
    const auto& s = detail::state(out);
    const std::string bullet = s.look.unicode ? "\xC2\xB7" : "-"; // ·
    const std::string margin(s.indent, ' ');
    detail::lines(out, margin + detail::paint(s.look, "90", bullet) + ' ', s.indent + 2, detail::ascii(value), tone::muted);
}
// Status line with a coloured badge, e.g. "[ OK ] Configuration applied".
inline void status(level l, const std::string& message, std::ostream& out) {
    const std::string prefix = "  " + detail::badge(out, l) + "  ";
    const tone t = l == level::fail ? tone::bad : l == level::warn ? tone::warn : tone::plain;
    detail::lines(out, prefix, 10, detail::ascii(message), t);
}
inline void blank(std::ostream& out) { out << '\n'; }

// Fixed-width table. Columns are left aligned unless the header starts with '>'.
// row_tones colour a whole row, or only tone_column when one is given.
inline void table(const std::vector<std::string>& headers, const std::vector<std::vector<std::string>>& rows,
                  std::ostream& out, const std::vector<tone>& row_tones = {}, size_t tone_column = static_cast<size_t>(-1)) {
    std::vector<size_t> w(headers.size());
    std::vector<bool> right(headers.size());
    std::vector<std::string> names(headers.size());
    for (size_t c = 0; c < headers.size(); ++c) {
        right[c] = !headers[c].empty() && headers[c][0] == '>';
        names[c] = right[c] ? headers[c].substr(1) : headers[c];
        w[c] = names[c].size();
        for (const auto& r : rows) if (c < r.size()) w[c] = std::max(w[c], r[c].size());
    }
    const auto& s = detail::state(out);
    const std::string margin(s.indent, ' ');
    auto cell = [&](size_t c, const std::string& v) {
        const std::string pad(w[c] - std::min(w[c], v.size()), ' ');
        return right[c] ? pad + v : v + pad;
    };
    std::string header_line, rule;
    for (size_t c = 0; c < names.size(); ++c) {
        header_line += (c ? "  " : "") + cell(c, names[c]);
        rule += (c ? "  " : "") + detail::repeat(s.look.unicode ? "\xE2\x94\x80" : "-", w[c]);
    }
    out << margin << detail::paint(s.look, "1", header_line) << '\n';
    out << margin << detail::paint(s.look, "90", rule) << '\n';
    for (size_t r = 0; r < rows.size(); ++r) {
        const tone t = r < row_tones.size() ? row_tones[r] : tone::plain;
        std::string line;
        for (size_t c = 0; c < names.size(); ++c) {
            const std::string value = c < rows[r].size() ? rows[r][c] : "";
            std::string text = cell(c, value);
            if (c == tone_column) {   // colour the value only, never its padding
                const std::string pad(w[c] - std::min(w[c], value.size()), ' ');
                text = right[c] ? pad + detail::paint(out, t, value) : detail::paint(out, t, value) + pad;
            }
            line += (c ? "  " : "") + text;
        }
        out << margin << (tone_column < names.size() ? line : detail::paint(out, t, line)) << '\n';
    }
}

// --- composite views ---------------------------------------------------------
inline void banner(std::ostream& out) {
    static constexpr std::array<const char*, 5> wordmark{{
        R"(   ____                  _ ______                 _____ ____  ____)",
        R"(  / __ \____ ___  ____  (_) ____/___  ________    / ___// __ \/ __ \)",
        R"( / / / / __ `__ \/ __ \/ / /   / __ \/ ___/ _ \   \__ \/ / / / /_/ /)",
        R"(/ /_/ / / / / / / / / / / /___/ /_/ / /  /  __/  ___/ / /_/ / _, _/)",
        R"(\____/_/ /_/ /_/_/ /_/_/\____/\____/_/   \___/  /____/_____/_/ |_|)"
    }};
    const auto& st = detail::look(out);
    out << '\n';
    for (const auto* line : wordmark) out << "  " << detail::paint(st, "96", line) << '\n';
    out << "  " << detail::paint(st, "1", "OmniCore Host " OCH_VERSION)
        << detail::paint(st, "90", "  |  OmniCoreSDR control and IQ streaming  |  ABI 1") << '\n';
}
// Compact header used at the top of single-purpose commands.
inline void title(const std::string& command, const std::string& subtitle, std::ostream& out) {
    const auto& st = detail::look(out);
    out << '\n' << "  " << detail::paint(st, "1;96", "OmniCore Host") << ' '
        << detail::paint(st, "90", OCH_VERSION) << "  "
        << detail::paint(st, "1", detail::ascii(command));
    if (!subtitle.empty()) out << detail::paint(st, "90", "  " + detail::ascii(subtitle));
    out << '\n';
}

inline void device(const och_device_info& i, bool detailed, std::ostream& out) {
    if (detailed) banner(out);
    section(detailed ? "Device" : "Discovered device", out);
    group("Identity", out);
    field("Name", detail::nonempty(detail::text(i.name)), out, tone::accent);
    field("Serial", detail::nonempty(detail::text(i.serial)), out);
    field("Driver", detail::nonempty(detail::text(i.driver)), out);
    if (detailed) {
        field("Firmware", detail::nonempty(detail::text(i.firmware)), out);
        field("Host ABI", i.abi_version, out);
    }
    group("USB link", out);
    if (i.usb_speed_mbps) {
        const tone t = i.usb_speed_mbps >= 5000 ? tone::good : tone::warn;
        const auto speed = i.usb_speed_mbps >= 1000
            ? detail::fixed(static_cast<double>(i.usb_speed_mbps) / 1000, 1) + " Gbit/s"
            : std::to_string(i.usb_speed_mbps) + " Mbit/s";
        field("Negotiated speed", speed + "  (" +
              (i.usb_speed_mbps >= 10000 ? "SuperSpeed+" : i.usb_speed_mbps >= 5000 ? "SuperSpeed" :
               i.usb_speed_mbps >= 480 ? "High-Speed, USB 2.0" : "Full-Speed, unsupported") + ")", out, t);
    } else field("Negotiated speed", "Not queried - use och info", out, tone::muted);
    group("RF capabilities", out);
    field("RX / TX channels", std::to_string(i.rx_channels) + " / " + std::to_string(i.tx_channels), out);
    field("RF sample-rate ceiling", detail::rate(i.max_sample_rate), out);
    if (detailed) field("Hardware timestamps", detail::binary(i.hardware_timestamps), out,
                        i.hardware_timestamps ? tone::good : tone::muted);
    if (detailed) {
        end_group(out); out << '\n';
        note("Link speed is the negotiated rate, not measured application throughput.", out);
        note("RX/TX share one sample clock and one USB link; four channels at the maximum rate are not implied.", out);

    }
    end_group(out);
}
inline void usb2_stream_reference(const och_device_info& i, const och_config& c, std::ostream& out) {
    if (i.usb_speed_mbps != 480) return;
    const auto* e = usb2_reference::find(i, c);
    if (!e) {
        note("No USB2 reference for this channel/format/firmware combination; validate with bench.", out);
        return;
    }
    field("USB2 tested reference", detail::fixed(e->rate_hz / 1e6, 3) + " MSPS/ch, " + std::to_string(e->seconds) + " s", out);
    if (c.sample_rate > e->rate_hz)
        status(level::warn, "Requested rate exceeds this USB2 test reference; reduce --sample-rate if faults occur.", out);
    note("Reference-host measurement only; this run's counters determine its result.", out);
}
inline void devices(const std::vector<och_device_info>& list, std::ostream& out) {
    section("Device discovery", out);
    if (list.empty()) {
        out << '\n';
        status(level::warn, "No OmniCoreSDR device found.", out);
        note("Check the USB data cable and that the device uses the libusbK driver.", out);
        note("Run och-driver check for driver diagnostics.", out);
        return;
    }
    out << '\n';
    std::vector<std::vector<std::string>> rows;
    size_t n = 0;
    for (const auto& i : list) {
        rows.push_back({std::to_string(++n), detail::nonempty(detail::ascii(detail::text(i.serial))),
                        detail::nonempty(detail::ascii(detail::text(i.name))), detail::nonempty(detail::ascii(detail::text(i.driver))),
                        std::to_string(i.rx_channels) + "x" + std::to_string(i.tx_channels),
                        detail::fixed(i.max_sample_rate / 1e6, 2) + " MSPS"});
    }
    table({">#", "Serial", "Name", "Driver", "RXxTX", ">RF max"}, rows, out);
    out << '\n';
    status(level::ok, std::to_string(list.size()) + (list.size() == 1 ? " device found." : " devices found."), out);
    note("Details and USB link speed: och info" + std::string(list.size() > 1 ? " --serial SERIAL" : ""), out);
}
inline void power(const och_power_status& p, std::ostream& out) {
    section("USB-C power (startup snapshot)", out);
    const bool valid = p.snapshot_valid == 1 && p.conflicting_snapshots == 0;
    const bool sink = valid && p.attached_state == 2;
    field("Source", "HD3SS3220 startup log (not live)", out, tone::muted);
    field("Log endpoint EP83", detail::binary(p.log_endpoint_present), out, detail::binary_tone(p.log_endpoint_present));
    field("Snapshot valid", valid ? "Yes" : "No", out, valid ? tone::good : tone::warn);
    if (p.conflicting_snapshots) field("Conflicting snapshots", "Yes", out, tone::warn);
    field("Supports 3 A at startup", sink && p.supports_3a == 1 ? "Yes" : sink && p.supports_3a == 0 ? "No" : "Unknown", out,
          sink && p.supports_3a == 1 ? tone::good : sink && p.supports_3a == 0 ? tone::warn : tone::muted);
    if (valid) {
        const char* role = p.attached_state == 0 ? "unattached" : p.attached_state == 1 ? "DFP/source" :
            p.attached_state == 2 ? "UFP/sink" : p.attached_state == 3 ? "accessory" : "Unknown";
        field("Attached state", std::string(role) + "  (" + std::to_string(p.attached_state) + ")", out);
        field("Current detected (raw)", p.current_detected, out);
        field("Source current", sink && p.source_current_ma >= 0 ? std::to_string(p.source_current_ma) + " mA" :
            "Unknown (USB default has no fixed mA value)", out);
        field("HD3SS3220 registers", "08=" + detail::hex(p.raw_08) + "  09=" + detail::hex(p.raw_09) + "  0A=" + detail::hex(p.raw_0a), out, tone::muted);
    }
    out << '\n';
    if (!valid) {
        if (p.conflicting_snapshots) status(level::warn, "Conflicting snapshots; no power conclusion is available.", out);
        else status(level::info, "No complete startup snapshot available.", out);
        note("An earlier och info/power call or another reader may already have consumed the EP83 log.", out);
    }
    note("Startup observation only; not a live current, voltage or LED readback.", out);
}
inline void config(const och_config& c, std::ostream& out) {
    section("RF configuration", out);
    group("Sampling", out);
    field("Sample rate", detail::rate(c.sample_rate), out, tone::accent);
    field("RX channels", detail::mask(c.rx_mask), out);
    field("TX channels", detail::mask(c.tx_mask), out);
    field("USB IQ precision", std::to_string(c.usb_iq_bits ? c.usb_iq_bits : 12) + "-bit" +
          (c.usb_iq_bits == 8 ? "  (sc8 wire)" : "  (packed12 wire)"), out);
    const char* agc = c.rx_agc == 0 ? "manual" : c.rx_agc == 1 ? "fast AGC" :
        c.rx_agc == 2 ? "slow AGC" : c.rx_agc == 3 ? "hybrid AGC" : "Unknown";
    group("Receive", out);
    field("LO frequency", detail::frequency(c.rx_frequency), out);
    field("Bandwidth", detail::fixed(c.rx_bandwidth / 1e6, 3) + " MHz", out);
    field("Gain mode", agc, out);
    field("Gain RX1 / RX2", detail::fixed(c.rx_gain[0], 2) + " / " + detail::fixed(c.rx_gain[1], 2) + " dB", out,
          c.rx_agc ? tone::muted : tone::plain);
    group("Transmit", out);
    field("LO frequency", detail::frequency(c.tx_frequency), out);
    field("Bandwidth", detail::fixed(c.tx_bandwidth / 1e6, 3) + " MHz", out);
    field("Attenuation TX1 / TX2", detail::fixed(c.tx_attenuation[0], 2) + " / " + detail::fixed(c.tx_attenuation[1], 2) + " dB", out);
    end_group(out); out << '\n';
    note("Last applied request; not a live readback of every RF register.", out);
}

// Throughput row for the benchmark table.
struct throughput_row {
    std::string direction;
    unsigned channels;
    uint32_t sample_rate;
    double seconds, usb_mbps, expected_mbps;
    uint64_t faults;
};
inline void throughput(const std::vector<throughput_row>& rows, std::ostream& out) {
    std::vector<std::vector<std::string>> cells;
    std::vector<tone> tones;
    for (const auto& r : rows) {
        const double ratio = r.expected_mbps > 0 ? r.usb_mbps / r.expected_mbps * 100 : 0;
        cells.push_back({r.direction, std::to_string(r.channels), detail::fixed(r.sample_rate / 1e6, 2),
                         detail::fixed(r.seconds, 2), detail::fixed(r.usb_mbps, 1), detail::fixed(r.expected_mbps, 1),
                         r.expected_mbps > 0 ? detail::fixed(ratio, 1) + " %" : "-", detail::grouped(r.faults)});
        tones.push_back(r.faults ? tone::warn : tone::muted);
    }
    table({"Dir", ">Ch", ">MSPS/ch", ">Time s", ">USB MB/s", ">Wire MB/s", ">Ratio", ">Faults"}, cells, out, tones, 7);
}

inline void stats(och_direction direction, const och_stats& s, unsigned channels_count,
                  uint32_t configured_rate, std::ostream& out, unsigned wire_bytes = 3, uint64_t startup_accepted = 0) {
    const bool tx = direction == OCH_TX;
    const double seconds = static_cast<double>(s.elapsed_ns) / 1e9;
    const uint64_t faults = stream_assessment::remaining_faults(s, !tx, startup_accepted);
    const bool hard = s.transfer_errors || s.host_dropped_samples || s.event_drops;
    const double expected = channels_count && configured_rate ?
        static_cast<double>(configured_rate) * channels_count * wire_bytes / 1e6 : 0;
    const double measured = seconds > 0 ? s.usb_bytes / seconds / 1e6 : 0;

    section(std::string(detail::direction(direction)) + " stream summary", out);
    out << '\n';
    if (!faults && expected > 0 && seconds >= 1 && measured < expected * 0.98)
        status(level::warn, "Throughput is below 98% of the requested wire rate.", out);
    else if (!faults && startup_accepted)
        status(level::ok, "Stable after " + std::to_string(startup_accepted) + " startup gap(s) in the first second.", out);
    else if (!faults) status(level::ok, "No faults.", out);
    else if (hard) status(level::fail, "Host-side loss or USB transfer errors.", out);
    else status(level::warn, "Device faults reported; see the counters and event log.", out);

    group("Overview", out);
    field("Rate", std::to_string(channels_count) + " ch x " + detail::fixed(configured_rate / 1e6, 3) + " MSPS", out);
    field("Elapsed", detail::fixed(seconds, 3) + " s" + (s.running ? "  (running)" : ""), out);
    field(tx ? "Samples accepted/ch" : "Samples delivered/ch", detail::grouped(s.samples), out);

    group("Throughput", out);
    field("USB average", seconds > 0 ? detail::fixed(measured) + " MB/s" : "Unknown (no elapsed time)", out, tone::accent);
    if (expected > 0 && seconds > 0)
        field("Of expected", detail::fixed(measured / expected * 100, 1) + " %  of " + detail::fixed(expected, 1) + " MB/s", out);
    field("Data volume", detail::bytes(s.usb_bytes), out);
    field("Host queue peak", detail::bytes(s.host_queue_peak_bytes), out);

    group("Faults", out);
    auto counter = [&](const char* label, uint64_t value, tone bad) {
        field(label, detail::grouped(value), out, value ? bad : tone::muted);
    };
    if (!tx) {
        counter("Device overflows (GAP)", s.device_overflows, tone::warn);
        counter("Host dropped samples", s.host_dropped_samples, tone::bad);
        counter("Header errors", s.resyncs, tone::warn);
    } else {
        counter("Device underflows", s.device_underflows, tone::warn);
        counter("Host starvation", s.tx_starvation_events, tone::warn);
        counter("Padding samples/ch", s.tx_padding_samples, tone::warn);
    }
    counter("USB transfer errors", s.transfer_errors, tone::bad);
    counter("Event queue drops", s.event_drops, tone::bad);
    end_group(out); out << '\n';

    note("MB/s decimal, whole run incl. start/stop; expected = " + std::to_string(wire_bytes) + " B/sample/ch.", out);
    if (tx) note("Accepted samples include prefill; USB completion is not DAC playback.", out);
}
inline void fir(const och_fir_config& f, std::ostream& out) {
    section(std::string(detail::direction(f.direction)) + " hardware FIR", out);
    field("Enabled", detail::binary(f.enabled), out, f.enabled == 1 ? tone::good : tone::muted);
    field("Verified snapshot", detail::binary(f.verified), out, f.verified == 1 ? tone::good : tone::warn);
    field("Physical channels", detail::mask(f.channel_mask), out);
    field(f.direction == OCH_RX ? "Decimation" : "Interpolation", "x" + std::to_string(f.factor), out);
    field("Taps / gain", std::to_string(f.tap_count) + " taps,  " + std::to_string(f.gain_db) + " dB", out);
    field("Coefficient CRC32", detail::hex(f.crc32, 8), out);
    if (f.tap_count > 128) { out << '\n'; status(level::warn, "Invalid tap count; showing the 128 available entries only.", out); }
    const size_t count = std::min<size_t>(f.tap_count, 128);
    if (count) {
        group("Coefficients", out);
        for (size_t start = 0; start < count; start += 8) {
            std::ostringstream values; values.imbue(std::locale::classic());
            values << detail::paint(out, tone::muted, (start < 10 ? "  " : start < 100 ? " " : "") + std::to_string(start) + " |");
            const size_t end = std::min(count, start + 8);
            for (size_t i = start; i < end; ++i) values << std::setw(7) << f.coefficients[i];
            out << "    " << values.str() << '\n';
        }
        end_group(out);
    }
    out << '\n';
    if (f.verified == 1) note("Coefficients come from the SDK's last SPI-verified snapshot.", out);
    else status(level::warn, "This configuration is not marked as a verified snapshot.", out);
}
inline void clocks(const och_clock_config& c, std::ostream& out) {
    section("AD9361 clock chain", out);
    field("Board reference", detail::frequency(c.reference_hz), out);
    field("RX / TX sample rate", detail::fixed(c.rx_hz[5] / 1e6, 6) + " / " + detail::fixed(c.tx_hz[5] / 1e6, 6) + " MSPS", out, tone::accent);
    out << '\n';
    const char* rx_names[] = {"BBPLL", "ADC", "R2", "R1", "RF", "Fs"};
    const char* tx_names[] = {"BBPLL", "DAC", "T2", "T1", "TF", "Fs"};
    std::vector<std::vector<std::string>> rows;
    for (size_t i = 0; i < 6; ++i)
        rows.push_back({std::to_string(i + 1), rx_names[i], detail::fixed(c.rx_hz[i] / 1e6, 6), tx_names[i], detail::fixed(c.tx_hz[i] / 1e6, 6)});
    table({">Stage", "RX node", ">RX MHz", "TX node", ">TX MHz"}, rows, out);
    out << '\n';
    if (c.rx_hz[0] != c.tx_hz[0] || c.rx_hz[5] != c.tx_hz[5])
        status(level::warn, "Reported BBPLL or sample rates differ between directions.", out);
    note("The AD9361 reference stays 40 MHz; an external 10 MHz disciplines the oscillator, otherwise PPS can steer it via the CH32 DAC.", out);
}
namespace detail {
// Value of key=... inside an SDK EVT line, or empty.
inline std::string evt_field(const std::string& s, const std::string& key) {
    for (size_t at = s.find(key + "="); at != std::string::npos; at = s.find(key + "=", at + 1)) {
        if (at && s[at - 1] != ' ') continue;
        const auto begin = at + key.size() + 1;
        return s.substr(begin, s.find(' ', begin) - begin);
    }
    return {};
}
// Raw SDK EVT lines lose their bookkeeping prefix (EVT, seq, stream, src).
inline std::string evt_body(const std::string& s) {
    std::istringstream in(s); std::string word, body;
    while (in >> word) {
        if (word == "EVT" || word.rfind("seq=", 0) == 0 || word.rfind("tseq=", 0) == 0 ||
            word.rfind("stream=", 0) == 0 || word.rfind("src=", 0) == 0) continue;
        body += (body.empty() ? "" : " ") + word;
    }
    return body;
}
inline std::string msps(const std::string& hz) {
    try { return fixed(std::stod(hz) / 1e6, 3); } catch (...) { return hz; }
}
}
// One line per event: elapsed time, severity, origin, plain-language text.
// summarized > 0 renders a roll-up line for that many further events of e.type.
inline void event(const och_event& e, double elapsed_seconds, std::ostream& out, uint64_t summarized = 0) {
    const std::string message = detail::text(e.message);
    const auto lowered = detail::lower(message);
    const std::string at = "  t=" + detail::grouped(e.hw_time);
    const char* tag = "EVENT";
    level l = level::info;
    std::string text;
    switch (e.type) {
    case OCH_EVENT_RX_GAP: tag = "RX GAP"; l = level::warn; text = "Overflow, next block GAP" + at; break;
    case OCH_EVENT_RX_LATE: tag = "RX LATE"; l = level::warn;
        text = lowered.find("ignored") != std::string::npos ? "Command ignored in this state"
                                                          : "Command late, started at once"; break;
    case OCH_EVENT_TX_BURST_ACK: tag = "TX ACK"; l = level::ok; text = "Burst complete" + at; break;
    case OCH_EVENT_TX_LATE: tag = "TX LATE"; l = level::warn; text = "Late start, burst dropped" + at; break;
    case OCH_EVENT_TX_UNDERFLOW: tag = "TX UNDERRUN"; l = level::warn; text = "FIFO empty mid-burst" + at; break;
    case OCH_EVENT_TX_ERROR: tag = "TX BLOCK"; l = level::fail;
        text = std::string(lowered.find("seq") != std::string::npos ? "Bad sequence" : "Bad header") +
               ", block " + std::to_string(e.block) + " dropped"; break;
    case OCH_EVENT_TIME_INVALIDATED: tag = "TIME"; l = level::warn;
        text = "Rate " + detail::msps(detail::evt_field(message, "old_rate")) + " -> " +
               detail::msps(detail::evt_field(message, "new_rate")) + " MSPS, time invalid"; break;
    case OCH_EVENT_HOST_OVERFLOW: tag = "HOST"; l = level::warn;
        text = lowered.find("ring full") != std::string::npos ? "Host ring full, samples dropped" : message; break;
    case OCH_EVENT_TX_STARVED: tag = "TX FEED"; l = level::warn; text = message; break;
    case OCH_EVENT_TRANSPORT: tag = "USB"; l = level::fail; text = message; break;
    case OCH_EVENT_RECONFIGURED: tag = "RECONFIG"; text = message; break;
    case OCH_EVENT_DEVICE: tag = "DEVICE"; text = detail::evt_body(message);
        if (lowered.rfind("err", 0) == 0 || lowered.find(" failed") != std::string::npos) l = level::fail;
        else if (lowered.find("overflow") != std::string::npos || lowered.find("underflow") != std::string::npos ||
                 lowered.find("warning") != std::string::npos) l = level::warn;
        break;
    default: text = message; break;
    }
    if (summarized) text = "+" + detail::grouped(summarized) + " more (summarized)";
    const auto& st = detail::look(out);
    std::string stamp = "+" + detail::fixed(elapsed_seconds, 3) + "s";
    if (stamp.size() < 9) stamp = std::string(9 - stamp.size(), ' ') + stamp;
    if (stamp.size() > 9) stamp = stamp.substr(0, 9);
    const std::string head = "  " + detail::paint(st, "90", stamp) + "  " + detail::badge(out, l) + "  ";
    std::string label = tag; label.resize(12, ' ');
    detail::lines(out, head + detail::paint(st, "1", label), 2 + 9 + 2 + 6 + 2 + 12,
                  detail::ascii(detail::nonempty(text, "(empty event message)")), l == level::fail ? tone::bad : tone::plain);
}

// Render an SDK key=value reply. Groups with many short fields use two
// columns so long status dumps stay scannable.
inline void reply(const std::string& title, const std::string& raw, std::ostream& out) {
    section(title, out);
    if (raw.empty()) { out << '\n'; note("(empty reply)", out); return; }
    struct item { std::string key, value; };
    std::vector<item> pending;
    bool grouped_now = false, started = false;
    auto start = [&] { if (!started) { out << '\n'; started = true; } };
    auto flush = [&] {
        if (pending.empty()) return;
        bool compact = pending.size() >= 4;
        for (const auto& i : pending) if (i.key.size() > 19 || i.value.size() > 14) { compact = false; break; }
        const size_t indent = detail::state(out).indent;
        start();
        if (!compact) {
            for (const auto& i : pending) field_text(i.key, i.value, out, detail::value_tone(i.key, i.value));
        } else {
            const std::string margin(indent, ' ');
            constexpr size_t key_w = 20, value_w = 15;
            for (size_t n = 0; n < pending.size(); n += 2) {
                std::string line = margin;
                for (size_t k = n; k < std::min(n + 2, pending.size()); ++k) {
                    const auto& i = pending[k];
                    line += i.key + std::string(key_w - i.key.size(), ' ');
                    auto v = detail::paint(out, detail::value_tone(i.key, i.value), i.value);
                    line += v;
                    if (k == n && n + 1 < pending.size()) line += std::string(value_w - i.value.size() + 2, ' ');
                }
                out << line << '\n';
            }
        }
        pending.clear();
    };
    auto open_group = [&](const std::string& name) { flush(); group(name, out); grouped_now = started = true; };
    std::istringstream input(raw); std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (detail::trim(line).empty()) continue;
        const auto keys = detail::keys(line);
        if (keys.empty()) {
            const auto trimmed = detail::trim(line);
            if (detail::status_group(trimmed)) open_group(trimmed);
            else { flush(); start(); note(line, out); }
            continue;
        }
        const auto prefix = detail::trim(line.substr(0, keys.front().start));
        if (!prefix.empty()) open_group(prefix);
        else {
            const auto first_key = line.substr(keys.front().start, keys.front().equals - keys.front().start);
            if (detail::status_group(first_key)) open_group(first_key);
        }
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto end = i + 1 < keys.size() ? keys[i + 1].start : line.size();
            const auto key = detail::ascii(line.substr(keys[i].start, keys[i].equals - keys[i].start));
            const auto value = detail::ascii(detail::trim(line.substr(keys[i].equals + 1, end - keys[i].equals - 1)));
            pending.push_back({key, value.empty() ? "(empty)" : value});
        }
        // Without explicit groups each line is its own block of fields.
        if (!grouped_now) flush();
    }
    flush();
    end_group(out);
}

// Single-line live progress for streaming commands. Returns the rendered text
// and its visible width (escape sequences excluded).
inline std::pair<std::string, size_t> progress(const style& st, double elapsed, double total,
                                               const std::vector<std::pair<std::string, double>>& rates) {
    std::ostringstream line; line.imbue(std::locale::classic());
    line << "  ";
    if (total > 0) {
        const double fraction = std::min(1.0, std::max(0.0, elapsed / total));
        constexpr size_t cells = 24;
        const size_t full = static_cast<size_t>(fraction * cells + 0.5);
        const std::string done = detail::repeat(st.unicode ? "\xE2\x96\x88" : "#", full);   // █
        const std::string todo = detail::repeat(st.unicode ? "\xE2\x96\x91" : ".", cells - full); // ░
        line << detail::paint(st, "36", done) << detail::paint(st, "90", todo) << ' '
             << std::setw(3) << static_cast<int>(fraction * 100) << "%  "
             << detail::fixed(elapsed, 1) << '/' << detail::fixed(total, 1) << " s";
    } else {
        line << detail::paint(st, "1;34", "RUN") << "  " << detail::fixed(elapsed, 1) << " s";
    }
    for (const auto& r : rates)
        line << "   " << detail::paint(st, "1", r.first) << ' ' << detail::fixed(r.second, 1) << " MB/s";
    const auto text = line.str();
    return {text, detail::columns(text)};
}
} // namespace och_cli::presentation
