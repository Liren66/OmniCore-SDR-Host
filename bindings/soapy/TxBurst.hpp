// TX burst bookkeeping for writeStream (SOAPY_PLAN.md 4.3). Pure logic on och flag values;
// tested offline in tests/interleave_test.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <omnicore/och.h>

namespace omnicore_soapy {
    class TxBurst {
    public:
        // Call after every activation.
        void reset() {
            open_ = false;
            queued_ = 0;
            ended_once_ = false;
        }

        // och_tx_metadata flags for one writeStream call. SoapySDR has no start-of-burst flag:
        // the first write after an END_BURST (or after activation) starts a new burst.
        uint32_t flags(bool has_time, bool end_burst) const {
            uint32_t f = 0;
            if (!open_) f |= OCH_TX_START_OF_BURST;
            if (has_time) f |= OCH_TX_HAS_TIME | OCH_TX_START_OF_BURST;
            if (end_burst) f |= OCH_TX_END_OF_BURST;
            return f;
        }

        // Records what och_send_burst accepted. Returns true when the burst was closed, which
        // happens only if every sample of an END_BURST write was accepted.
        bool accepted(size_t accepted, size_t requested, bool end_burst) {
            queued_ += accepted;
            if (end_burst && accepted == requested) {
                open_ = false;
                ended_once_ = true;
                return true;
            }
            if (accepted) open_ = true;
            return false;
        }

        // The core emits TX_STARVED whenever its queue runs empty, including the idle time
        // between bursts. Only an open burst running dry is an underflow.
        bool starved_is_underflow() const { return open_; }

        // The core starts a TX stream only once the FPGA holds about one block, so the plugin
        // starts it when one USB transfer is queued or a burst has been closed.
        bool should_start(size_t start_threshold) const { return queued_ >= start_threshold || ended_once_; }

        size_t queued() const { return queued_; }

    private:
        bool open_ = false;
        bool ended_once_ = false;
        size_t queued_ = 0; // samples per channel accepted since activation
    };
} // namespace omnicore_soapy
