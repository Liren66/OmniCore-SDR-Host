// mxArray <-> C++ value conversion for the gateway. Inputs are validated here and reported as
// omnicore:InvalidArgument; the +omnicore classes convert MATLAB strings to char vectors and
// logicals/numbers to double before calling the MEX function, so only those types arrive.
#pragma once
#include "Error.hpp"
#include <cmath>
#include <cstdint>
#include <mex.h>
#include <string>
#include <utility>
#include <vector>

namespace omnicore_mex {
    inline std::string to_string(const mxArray *a, const char *what) {
        if (!a || !mxIsChar(a) || (mxGetM(a) > 1 && mxGetNumberOfElements(a) > 0))
            throw invalid_argument(std::string(what) + " must be a character vector");
        char *s = mxArrayToUTF8String(a);
        if (!s) throw invalid_argument(std::string(what) + " must be a character vector");
        std::string out(s);
        mxFree(s);
        return out;
    }

    inline std::vector<double> to_vector(const mxArray *a, const char *what) {
        if (!a || !(mxIsDouble(a) || mxIsLogical(a)) || mxIsComplex(a) || mxIsSparse(a))
            throw invalid_argument(std::string(what) + " must be real numeric");
        const size_t n = mxGetNumberOfElements(a);
        std::vector<double> out(n);
        if (mxIsLogical(a)) {
            const mxLogical *p = mxGetLogicals(a);
            for (size_t i = 0; i < n; ++i) out[i] = p[i] ? 1 : 0;
        } else {
            const double *p = mxGetDoubles(a);
            for (size_t i = 0; i < n; ++i) out[i] = p[i];
        }
        for (double v: out)
            if (!std::isfinite(v)) throw invalid_argument(std::string(what) + " must be finite");
        return out;
    }

    inline double to_scalar(const mxArray *a, const char *what) {
        const auto v = to_vector(a, what);
        if (v.size() != 1) throw invalid_argument(std::string(what) + " must be a scalar");
        return v[0];
    }

    // Timeout arguments: seconds -> milliseconds for och calls.
    inline uint32_t timeout_ms(double seconds) {
        if (!(seconds > 0) || seconds > 3600) throw invalid_argument("Timeout must be more than 0 and at most 3600 s");
        return uint32_t(std::ceil(seconds * 1000));
    }

    inline bool to_bool(const mxArray *a, const char *what) { return to_scalar(a, what) != 0; }

    // Handles are uint64 scalars issued by the registry.
    inline uint64_t to_id(const mxArray *a) {
        if (!a || !mxIsUint64(a) || mxGetNumberOfElements(a) != 1)
            throw Error("omnicore:InvalidHandle", "expected an OmniCore handle (uint64 scalar)");
        return *mxGetUint64s(a);
    }

    inline mxArray *make_string(const std::string &s) { return mxCreateString(s.c_str()); }
    inline mxArray *make_double(double v) { return mxCreateDoubleScalar(v); }
    inline mxArray *make_bool(bool v) { return mxCreateLogicalScalar(v); }
    inline mxArray *make_id(uint64_t v) {
        mxArray *a = mxCreateNumericMatrix(1, 1, mxUINT64_CLASS, mxREAL);
        *mxGetUint64s(a) = v;
        return a;
    }
    inline mxArray *make_row(const std::vector<double> &v) {
        mxArray *a = mxCreateDoubleMatrix(1, v.size(), mxREAL);
        double *p = mxGetDoubles(a);
        for (size_t i = 0; i < v.size(); ++i) p[i] = v[i];
        return a;
    }

    // Builds a 1x1 struct whose fields keep the order they were added in.
    class StructBuilder {
    public:
        StructBuilder &add(const char *name, mxArray *value) {
            fields_.emplace_back(name, value);
            return *this;
        }
        mxArray *build() {
            std::vector<const char *> names;
            for (auto &f: fields_) names.push_back(f.first);
            mxArray *s = mxCreateStructMatrix(1, 1, int(names.size()), names.data());
            for (size_t i = 0; i < fields_.size(); ++i) mxSetFieldByNumber(s, 0, int(i), fields_[i].second);
            fields_.clear();
            return s;
        }

    private:
        std::vector<std::pair<const char *, mxArray *>> fields_;
    };
} // namespace omnicore_mex
