#pragma once

#include <omnicore/och.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace och::core {

    // EP83 is a consumable, best-effort startup log, not a live Type-C query.
    // The caller serializes feed/status. No allocations, I/O or USB state changes.
    class PowerLogParser {
        static constexpr std::string_view raw_prefix = "HD3SS3220:";
        static constexpr std::string_view field_08 = "HD3SS3220: 08=";
        static constexpr std::string_view field_09 = " 09=";
        static constexpr std::string_view field_0a = " 0A=";
        static constexpr std::size_t line_capacity = 256;
        std::array<char, line_capacity> line_{};
        std::size_t used_ = 0;
        bool discarding_ = false, poisoned_ = false, have_raw_ = false;
        std::array<std::uint8_t, 3> first_raw_{};
        och_power_status state_ = empty_status();

        static och_power_status empty_status() noexcept {
            och_power_status s{};
            s.struct_size = sizeof s;
            s.source_current_ma = -1;
            s.supports_3a = -1;
            return s;
        }
        static void make_unknown(och_power_status &s) noexcept {
            const auto conflict = s.conflicting_snapshots;
            s = empty_status();
            s.conflicting_snapshots = conflict;
        }
        static int hex(char c) noexcept {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        }
        bool is_raw_line() const noexcept {
            return used_ >= raw_prefix.size() && std::memcmp(line_.data(), raw_prefix.data(), raw_prefix.size()) == 0;
        }
        void invalidate() noexcept {
            poisoned_ = true;
            make_unknown(state_);
        }
        void accept(const std::array<std::uint8_t, 3> &raw) noexcept {
            if (have_raw_ && raw != first_raw_) {
                state_.conflicting_snapshots = 1;
                invalidate();
                return;
            }
            if (have_raw_) return; // Identical replay cannot repair an explicit bad line.
            first_raw_ = raw;
            have_raw_ = true;
            if (poisoned_) return;
            state_.snapshot_valid = 1;
            state_.raw_08 = raw[0];
            state_.raw_09 = raw[1];
            state_.raw_0a = raw[2];
            state_.attached_state = (raw[1] >> 6) & 3U;
            state_.current_detected = (raw[0] >> 4) & 3U;
            if (state_.attached_state == 2) {
                constexpr std::int32_t current_ma[] = {-1, 1500, 500, 3000};
                state_.source_current_ma = current_ma[state_.current_detected];
                state_.supports_3a = state_.current_detected == 3 ? 1 : 0;
            }
        }
        void finish_line() noexcept {
            if (!is_raw_line()) return; // Never search for a header embedded in noise.
            auto n = used_;
            if (n && line_[n - 1] == '\r') --n;
            constexpr auto pos_08 = field_08.size();
            constexpr auto pos_09 = pos_08 + 2 + field_09.size();
            constexpr auto pos_0a = pos_09 + 2 + field_0a.size();
            if (n != pos_0a + 2 || std::memcmp(line_.data(), field_08.data(), field_08.size()) != 0 ||
                std::memcmp(line_.data() + pos_08 + 2, field_09.data(), field_09.size()) != 0 ||
                std::memcmp(line_.data() + pos_09 + 2, field_0a.data(), field_0a.size()) != 0) {
                invalidate();
                return;
            }
            std::array<std::uint8_t, 3> raw{};
            constexpr std::size_t positions[] = {pos_08, pos_09, pos_0a};
            for (unsigned i = 0; i < raw.size(); ++i) {
                const int hi = hex(line_[positions[i]]), lo = hex(line_[positions[i] + 1]);
                if (hi < 0 || lo < 0) {
                    invalidate();
                    return;
                }
                raw[i] = static_cast<std::uint8_t>((hi << 4) | lo);
            }
            accept(raw);
        }

    public:
        void feed(const std::uint8_t *data, std::size_t size) noexcept {
            if (!data) {
                if (size) invalidate();
                return;
            }
            for (std::size_t i = 0; i < size; ++i) {
                const char c = static_cast<char>(data[i]);
                if (c == '\n') {
                    if (!discarding_) finish_line();
                    used_ = 0;
                    discarding_ = false;
                } else if (!discarding_) {
                    if (used_ == line_capacity) {
                        if (is_raw_line()) invalidate();
                        discarding_ = true; // Retain prefix; discard the entire suffix through LF.
                    } else
                        line_[used_++] = c;
                }
            }
        }

        och_power_status status() const noexcept {
            auto result = state_;
            // A bounded host read may end halfway through a newer snapshot. Do not
            // keep claiming the old 3 A result while an explicit raw line is pending.
            // Ordinary partial debug text, including "HD3SS3220 @ ...", is harmless.
            if (is_raw_line()) make_unknown(result);
            return result;
        }
    };

} // namespace och::core
