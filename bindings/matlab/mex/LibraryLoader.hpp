#pragma once

namespace omnicore_mex {
    // Throws omnicore:LibraryNotFound when the OmniCore Host library cannot be loaded.
    void ensure_library_loaded();
} // namespace omnicore_mex
