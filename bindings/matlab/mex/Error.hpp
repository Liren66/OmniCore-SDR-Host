// Errors and warnings of the MEX gateway (MEX_PLAN.md 2.2).
//
// mexErrMsgIdAndTxt and mexWarnMsgIdAndTxt (when the user turned that warning into an error)
// longjmp out of the MEX function and skip C++ destructors. So the gateway never calls them while
// C++ objects are alive: errors travel as exceptions and warnings are collected, and
// omnicore_mex.cpp reports both after the dispatch has returned or unwound.
#pragma once
#include <omnicore/och.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace omnicore_mex {
    // An error with a MATLAB identifier ("omnicore:BUSY").
    class Error : public std::runtime_error {
    public:
        Error(std::string id, const std::string &message) : std::runtime_error(message), id_(std::move(id)) {}
        const std::string &id() const { return id_; }

    private:
        std::string id_;
    };

    inline const char *result_name(int rc) {
        switch (rc) {
        case OCH_TIMEOUT:
            return "TIMEOUT";
        case OCH_INVALID:
            return "INVALID";
        case OCH_NOT_FOUND:
            return "NOT_FOUND";
        case OCH_BUSY:
            return "BUSY";
        case OCH_IO:
            return "IO";
        case OCH_UNSUPPORTED:
            return "UNSUPPORTED";
        case OCH_STOPPED:
            return "STOPPED";
        case OCH_BUFFER_SMALL:
            return "BUFFER_SMALL";
        case OCH_PROTOCOL:
            return "PROTOCOL";
        default:
            return "Failed";
        }
    }

    // Throws omnicore:<RESULT> with och_last_error() when rc is not OCH_OK.
    inline void check(int rc, const char *what) {
        if (rc == OCH_OK) return;
        const char *detail = och_last_error();
        throw Error(std::string("omnicore:") + result_name(rc),
                    std::string("OmniCore ") + what + ": " +
                            (detail && *detail ? detail : "error " + std::to_string(rc)));
    }

    // Like check, but returns false for OCH_UNSUPPORTED (firmware without the feature).
    inline bool check_supported(int rc, const char *what) {
        if (rc == OCH_UNSUPPORTED) return false;
        check(rc, what);
        return true;
    }

    inline Error invalid_argument(const std::string &message) { return Error("omnicore:InvalidArgument", message); }

    // Warnings raised during one MEX call; issued by the gateway after the call.
    struct Warning {
        std::string id, message;
    };
    using Warnings = std::vector<Warning>;

    template<class T>
    T och_struct() {
        T s{};
        s.struct_size = sizeof s;
        return s;
    }
} // namespace omnicore_mex
