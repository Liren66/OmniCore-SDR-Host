// Ctrl+C inside long MEX calls (MEX_PLAN.md 5.5).
//
// MATLAB handles Ctrl+C only between MEX calls, so a receive of many seconds cannot be stopped.
// With OMNICORE_MEX_CTRL_C (CMake OCH_MATLAB_CTRL_C, off by default; MEX_PLAN.md 10.2) the waits
// check MATLAB's interrupt flag through utIsInterruptPending: undocumented, exported by libut
// unchanged for many releases, but not guaranteed. The call then ends with omnicore:Interrupted;
// the RX / TX streams it used are stopped as usual. Without it, waits still come in slices of at
// most wait_slice_ms so the check costs nothing to add later.
#pragma once
#include "Error.hpp"
#include <cstdint>

#ifdef OMNICORE_MEX_CTRL_C
extern "C" bool utIsInterruptPending(void);
#endif

namespace omnicore_mex {
    constexpr uint32_t wait_slice_ms = 100;

    inline bool interrupt_pending() {
#ifdef OMNICORE_MEX_CTRL_C
        return utIsInterruptPending();
#else
        return false;
#endif
    }

    inline void check_interrupt() {
        if (interrupt_pending()) throw Error("omnicore:Interrupted", "OmniCore: interrupted by Ctrl+C");
    }
} // namespace omnicore_mex
