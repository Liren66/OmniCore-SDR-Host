// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace och_cli {
inline std::string json(const std::string& value) {
    static const char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 32) { out += "\\u00"; out += hex[ch >> 4]; out += hex[ch & 15]; }
            else out += static_cast<char>(ch);
        }
    }
    return out + "\"";
}
inline double number(const std::string& text) {
    if (text.empty()) throw std::invalid_argument("empty numeric value");
    for (unsigned char c : text)
        if (c <= ' ') throw std::invalid_argument("whitespace in numeric value: " + text);
    double multiplier = 1;
    std::string value = text;
    switch (value.back()) {
    case 'k': case 'K': multiplier = 1e3; value.pop_back(); break;
    case 'M': multiplier = 1e6; value.pop_back(); break;
    case 'G': multiplier = 1e9; value.pop_back(); break;
    }
    // Decimal/scientific notation only, never hex, NaN, Inf or partial parses.
    size_t p = 0;
    if (p < value.size() && (value[p] == '+' || value[p] == '-')) ++p;
    bool digit = false;
    while (p < value.size() && value[p] >= '0' && value[p] <= '9') { digit = true; ++p; }
    if (p < value.size() && value[p] == '.') {
        ++p;
        while (p < value.size() && value[p] >= '0' && value[p] <= '9') { digit = true; ++p; }
    }
    if (!digit) throw std::invalid_argument("invalid decimal number: " + text);
    if (p < value.size() && (value[p] == 'e' || value[p] == 'E')) {
        ++p;
        if (p < value.size() && (value[p] == '+' || value[p] == '-')) ++p;
        size_t begin = p;
        while (p < value.size() && value[p] >= '0' && value[p] <= '9') ++p;
        if (p == begin) throw std::invalid_argument("invalid exponent: " + text);
    }
    if (p != value.size()) throw std::invalid_argument("invalid numeric suffix/value: " + text);
    char* end = nullptr;
    errno = 0;
    double result = std::strtod(value.c_str(), &end) * multiplier;
    if (!end || *end || errno == ERANGE || !std::isfinite(result)) throw std::invalid_argument("numeric value out of range: " + text);
    return result;
}
inline uint64_t integer(const std::string& text, uint64_t maximum) {
    double n = number(text);
    if (n < 0 || n > static_cast<double>(maximum) || std::floor(n) != n)
        throw std::invalid_argument("expected non-negative integer in range: " + text);
    return static_cast<uint64_t>(n);
}
inline uint32_t channels(const std::string& text) {
    if (text == "1") return 1;
    if (text == "2") return 2;
    if (text == "1,2") return 3;
    throw std::invalid_argument("channels must be 1, 2 or 1,2");
}
struct options {
    std::string command;
    std::map<std::string, std::string> values;
    std::set<std::string> flags;
    std::vector<std::string> positional;
    bool has(const std::string& key) const { return values.count(key) || flags.count(key); }
    std::string get(const std::string& key, const std::string& fallback = {}) const {
        auto it = values.find(key); return it == values.end() ? fallback : it->second;
    }
};
inline options parse(int argc, char** argv) {
    const std::set<std::string> flag_names = {"json", "help", "rx", "tx", "loop", "overwrite", "coefficients-only", "no-color", "fail-fast", "no-mcs", "loopback", "master-ext-ref"};
    const std::set<std::string> value_names = {"serial", "sample-rate", "channels", "rx-channels", "tx-channels",
        "seconds", "file", "format", "tone", "amplitude", "lo-frequency", "rx-lo-frequency", "tx-lo-frequency",
        "bandwidth", "rx-bw", "tx-bw", "gain", "agc", "attenuation", "buffer-mib", "queue", "block", "rx-chain", "tx-chain", "iq-bits", "count", "interval", "devices", "mode", "monitor", "start-time"};
    const std::map<std::string, std::string> aliases = {
        {"rate", "sample-rate"}, {"frequency", "lo-frequency"},
        {"rx-freq", "rx-lo-frequency"}, {"tx-freq", "tx-lo-frequency"}};
    options out;
    bool positional_only = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--" && !positional_only) { positional_only = true; continue; }
        if (!positional_only && arg.rfind("--", 0) == 0) {
            auto eq = arg.find('=');
            std::string key = arg.substr(2, eq == std::string::npos ? eq : eq - 2);
            auto alias = aliases.find(key);
            if (alias != aliases.end()) key = alias->second;
            if (out.has(key)) throw std::invalid_argument("duplicate or conflicting option --" + key);
            if (flag_names.count(key)) {
                if (eq != std::string::npos) throw std::invalid_argument("flag takes no value: --" + key);
                out.flags.insert(key);
            } else if (value_names.count(key)) {
                if (eq != std::string::npos) out.values[key] = arg.substr(eq + 1);
                else {
                    if (++i == argc || std::string(argv[i]).rfind("--", 0) == 0)
                        throw std::invalid_argument("missing value for --" + key);
                    out.values[key] = argv[i];
                }
                if (out.values[key].empty()) throw std::invalid_argument("empty value for --" + key);
            } else throw std::invalid_argument("unknown option --" + key);
        } else if (out.command.empty()) out.command = arg;
        else out.positional.push_back(arg);
    }
    if (out.command.empty()) out.command = "help";
    return out;
}
inline void allowed(const options& opt, std::set<std::string> names, bool positional = false) {
    names.insert("serial"); names.insert("json"); names.insert("help"); names.insert("no-color");
    for (const auto& p : opt.values) if (!names.count(p.first))
        throw std::invalid_argument("--" + p.first + " is not valid for " + opt.command);
    for (const auto& p : opt.flags) if (!names.count(p))
        throw std::invalid_argument("--" + p + " is not valid for " + opt.command);
    if (!positional && !opt.positional.empty()) throw std::invalid_argument("unexpected positional argument: " + opt.positional.front());
}
} // namespace och_cli
