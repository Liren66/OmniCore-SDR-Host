#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <omnicore/och.h>

namespace och::core {
    // Single producer/single consumer ownership. Producer never overwrites a read
    // lease. RX chooses drop-new on saturation, while TX applies backpressure.
    // Storage and descriptors are allocated/touched before streaming begins.
    class Ring {
    public:
        struct Block {
            std::unique_ptr<uint8_t[]> data;
            size_t size = 0;
            och_metadata meta{};
        };

        Ring(size_t blocks, size_t bytes) : blocks_(blocks), bytes_(bytes) {
            if (blocks < 2 || !bytes) throw std::invalid_argument("ring dimensions");
            for (auto &b: blocks_) {
                b.data = std::make_unique<uint8_t[]>(bytes);
                std::memset(b.data.get(), 0, bytes);
            }
        }

        Block *write() noexcept {
            auto w = write_.load(std::memory_order_relaxed);
            return w - read_.load(std::memory_order_acquire) < blocks_.size() ? &blocks_[w % blocks_.size()] : nullptr;
        }

        void publish(size_t size, const och_metadata &meta) noexcept {
            std::lock_guard<std::mutex> lock(wait_);
            auto w = write_.load(std::memory_order_relaxed);
            auto &b = blocks_[w % blocks_.size()];
            b.size = size;
            b.meta = meta;
            queued_.fetch_add(size, std::memory_order_relaxed);
            write_.store(w + 1, std::memory_order_release);
            cv_.notify_one();
        }

        Block *read() noexcept {
            auto r = read_.load(std::memory_order_relaxed);
            return r != write_.load(std::memory_order_acquire) ? &blocks_[r % blocks_.size()] : nullptr;
        }

        void release() noexcept {
            std::lock_guard<std::mutex> lock(wait_);
            auto r = read_.load(std::memory_order_relaxed);
            queued_.fetch_sub(blocks_[r % blocks_.size()].size, std::memory_order_release);
            read_.store(r + 1, std::memory_order_release);
            cv_.notify_one();
        }

        bool wait_read(uint32_t ms, const std::atomic<bool> &stop, const std::atomic<bool> *interrupt = nullptr) {
            std::unique_lock<std::mutex> lock(wait_);
            return cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] {
                return stop.load() || (interrupt && interrupt->load()) || read() != nullptr;
            }) && read();
        }

        bool wait_write(uint32_t ms, const std::atomic<bool> &stop, const std::atomic<bool> *interrupt = nullptr) {
            std::unique_lock<std::mutex> lock(wait_);
            return cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] {
                return stop.load() || (interrupt && interrupt->load()) || write() != nullptr;
            }) && write();
        }

        // Both endpoints must be stopped and no outstanding read lease may exist.
        void reset() noexcept {
            read_ = 0;
            write_ = 0;
            queued_ = 0;
        }

        // Call after setting the external stop flag. Holding wait_ closes the
        // predicate-check -> wait transition: a stop notification cannot slip
        // between those steps and leave a long-timeout waiter asleep.
        void wake() noexcept {
            std::lock_guard<std::mutex> lock(wait_);
            cv_.notify_all();
        }

        uint64_t queued() const noexcept { return queued_.load(std::memory_order_acquire); }
        size_t block_bytes() const noexcept { return bytes_; }

    private:
        std::vector<Block> blocks_;
        size_t bytes_;
        alignas(64) std::atomic<uint64_t> write_{0};
        alignas(64) std::atomic<uint64_t> read_{0};
        alignas(64) std::atomic<uint64_t> queued_{0};
        std::mutex wait_;
        std::condition_variable cv_;
    };
}
