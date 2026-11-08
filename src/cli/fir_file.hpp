// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <omnicore/och.h>
#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace och_cli {
    struct fir_file {
        och_fir_config config{};
        std::vector<std::string> ignored_profile_fields;
    };
    namespace fir_detail {
        inline std::string trim(std::string text) {
            auto first = text.find_first_not_of(" \t\r");
            if (first == std::string::npos) return {};
            auto last = text.find_last_not_of(" \t\r");
            return text.substr(first, last - first + 1);
        }
        [[noreturn]] inline void fail(size_t line, const std::string &message) {
            throw std::invalid_argument("FTR line " + std::to_string(line) + ": " + message);
        }
        inline int64_t decimal(const std::string &text, int64_t minimum, int64_t maximum, size_t line) {
            if (text.empty()) fail(line, "empty decimal integer");
            const char *first = text.data();
            const char *last = first + text.size();
            if (*first == '+') {
                ++first;
                if (first == last || *first < '0' || *first > '9') fail(line, "invalid decimal integer: " + text);
            }
            if (first == last) fail(line, "invalid decimal integer: " + text);
            int64_t value = 0;
            auto parsed = std::from_chars(first, last, value, 10);
            if (parsed.ec != std::errc() || parsed.ptr != last || value < minimum || value > maximum)
                fail(line, "decimal integer outside allowed range: " + text);
            return value;
        }
        inline std::vector<std::string> words(const std::string &text) {
            std::istringstream input(text);
            input.imbue(std::locale::classic());
            std::vector<std::string> result;
            std::string token;
            while (input >> token) result.push_back(token);
            return result;
        }
        struct header {
            bool present = false;
            uint32_t mask = 0, factor = 0;
            int32_t gain = 0;
        };
    }

    // FTR coefficients are TX first, RX second. A one-column file supplies the
    // same coefficients for its declared direction(s). Full clock profiles require
    // explicit coefficients_only because this loader does not apply their clocks.
    inline fir_file parse_ftr(std::string text, och_direction direction, bool coefficients_only = false) {
        using namespace fir_detail;
        if (direction != OCH_RX && direction != OCH_TX) throw std::invalid_argument("invalid FIR direction");
        if (text.size() > 256u * 1024) throw std::invalid_argument("FTR file exceeds 256 KiB");
        if (text.rfind("\xef\xbb\xbf", 0) == 0) text.erase(0, 3);
        for (unsigned char c: text)
            if ((c < 32 && c != '\n' && c != '\r' && c != '\t') || c == 127)
                throw std::invalid_argument("FTR file contains a binary/control character");
        header rx, tx;
        std::vector<std::array<int16_t, 2>> rows;
        std::set<std::string> profile_seen;
        fir_file result;
        result.config.struct_size = sizeof(result.config);
        result.config.direction = direction;
        std::istringstream input(text);
        input.imbue(std::locale::classic());
        std::string line_text;
        size_t line_number = 0, columns = 0;
        while (std::getline(input, line_text)) {
            ++line_number;
            if (line_text.size() > 4096) fail(line_number, "line exceeds 4096 bytes");
            auto comment = line_text.find('#');
            if (comment != std::string::npos) line_text.resize(comment);
            line_text = trim(std::move(line_text));
            if (line_text.empty()) continue;
            auto tokens = words(line_text);
            if (tokens[0] == "RX" || tokens[0] == "TX") {
                if (!rows.empty()) fail(line_number, "header must precede coefficients");
                bool is_rx = tokens[0] == "RX";
                auto &h = is_rx ? rx : tx;
                if (h.present) fail(line_number, "duplicate direction header");
                if (tokens.size() != 6 || tokens[2] != "GAIN" || tokens[4] != (is_rx ? "DEC" : "INT"))
                    fail(line_number, "expected RX mask GAIN dB DEC factor or TX mask GAIN dB INT factor");
                h.mask = static_cast<uint32_t>(decimal(tokens[1], 1, 3, line_number));
                h.gain = static_cast<int32_t>(decimal(tokens[3], -12, 6, line_number));
                h.factor = static_cast<uint32_t>(decimal(tokens[5], 1, 4, line_number));
                if (h.factor != 1 && h.factor != 2 && h.factor != 4) fail(line_number, "FIR factor must be 1, 2 or 4");
                if (is_rx ? (h.gain != -12 && h.gain != -6 && h.gain != 0 && h.gain != 6)
                          : (h.gain != -6 && h.gain != 0))
                    fail(line_number, is_rx ? "RX gain must be -12, -6, 0 or 6 dB" : "TX gain must be -6 or 0 dB");
                h.present = true;
                continue;
            }
            if (tokens[0] == "RTX" || tokens[0] == "RRX" || tokens[0] == "BWTX" || tokens[0] == "BWRX") {
                if (!rows.empty()) fail(line_number, "profile metadata must precede coefficients");
                if (!profile_seen.insert(tokens[0]).second) fail(line_number, "duplicate profile metadata");
                size_t expected = (tokens[0] == "RTX" || tokens[0] == "RRX") ? 7 : 2;
                if (tokens.size() != expected) fail(line_number, "wrong profile field count");
                for (size_t i = 1; i < tokens.size(); ++i) (void) decimal(tokens[i], 1, UINT32_MAX, line_number);
                result.ignored_profile_fields.push_back(tokens[0]);
                continue;
            }
            if (!rx.present && !tx.present) fail(line_number, "direction header required before coefficients");
            if (rows.size() >= 128) fail(line_number, "more than 128 taps");
            auto comma = line_text.find(',');
            size_t row_columns = comma == std::string::npos ? 1 : 2;
            if (comma != std::string::npos && line_text.find(',', comma + 1) != std::string::npos)
                fail(line_number, "coefficient row must have one or two columns");
            if (columns && columns != row_columns) fail(line_number, "mixed one-column and two-column coefficients");
            columns = row_columns;
            std::array<int16_t, 2> row{};
            if (columns == 1)
                row[0] = row[1] = static_cast<int16_t>(decimal(line_text, -32768, 32767, line_number));
            else {
                if (!rx.present || !tx.present) fail(line_number, "two-column data requires both RX and TX headers");
                row[0] = static_cast<int16_t>(decimal(trim(line_text.substr(0, comma)), -32768, 32767, line_number));
                row[1] = static_cast<int16_t>(decimal(trim(line_text.substr(comma + 1)), -32768, 32767, line_number));
            }
            rows.push_back(row);
        }
        const auto &selected = direction == OCH_RX ? rx : tx;
        if (!selected.present) throw std::invalid_argument("FTR file does not declare the requested direction");
        if (selected.mask != 3)
            throw std::invalid_argument("this SDK requires FIR channel mask 3 (both physical channels)");
        if (rows.empty() || rows.size() % 16)
            throw std::invalid_argument("FIR tap count must be 16..128 and a multiple of 16");
        if (direction == OCH_TX && selected.factor == 1 && rows.size() > 64)
            throw std::invalid_argument("AD9361 TX interpolation 1 permits at most 64 taps");
        if (!profile_seen.empty() && !coefficients_only)
            throw std::invalid_argument(
                    "FTR contains clock/bandwidth profile metadata; use --coefficients-only to explicitly ignore it");
        result.config.channel_mask = selected.mask;
        result.config.factor = selected.factor;
        result.config.tap_count = static_cast<uint32_t>(rows.size());
        result.config.gain_db = selected.gain;
        result.config.enabled = 0;
        for (size_t i = 0; i < rows.size(); ++i) result.config.coefficients[i] = rows[i][direction == OCH_RX ? 1 : 0];
        return result;
    }
    inline fir_file read_ftr(const std::string &name, och_direction direction, bool coefficients_only = false) {
        auto path = std::filesystem::u8path(name);
        if (!std::filesystem::is_regular_file(path)) throw std::invalid_argument("FTR input is not a regular file");
        if (std::filesystem::file_size(path) > 256u * 1024) throw std::invalid_argument("FTR file exceeds 256 KiB");
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("cannot open FTR file: " + name);
        // Bound the actual read as well, in case the file grows after file_size().
        std::string data(256u * 1024 + 1, '\0');
        file.read(data.data(), static_cast<std::streamsize>(data.size()));
        if (file.bad()) throw std::runtime_error("FTR read failed: " + name);
        data.resize(static_cast<size_t>(file.gcount()));
        return parse_ftr(std::move(data), direction, coefficients_only);
    }
} // namespace och_cli
