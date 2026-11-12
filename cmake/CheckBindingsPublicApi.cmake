# Run as: cmake -DSOURCE_DIR=<repo> -P CheckBindingsPublicApi.cmake
# The bindings (SoapySDR module, MATLAB MEX gateway and their shared helpers in bindings/common)
# must build against an installed OmniCore Host, so they may include only the public C header
# <omnicore/och.h>, their host framework (SoapySDR, MATLAB), the standard library, their own files
# and "common/..." helpers.
file(GLOB_RECURSE sources "${SOURCE_DIR}/bindings/*.c" "${SOURCE_DIR}/bindings/*.cpp"
        "${SOURCE_DIR}/bindings/*.h" "${SOURCE_DIR}/bindings/*.hpp")
set(found "")
foreach (source IN LISTS sources)
    file(STRINGS "${source}" lines REGEX "^[ \t]*#[ \t]*include")
    foreach (line IN LISTS lines)
        if (line MATCHES "<omnicore/" AND NOT line MATCHES "<omnicore/och\\.h>")
            set(bad TRUE)
        elseif (line MATCHES "\"" AND (line MATCHES "\\.\\./" OR line MATCHES "[\"/](src|core|capi|transport|dsp|protocol)/"))
            set(bad TRUE)
        else ()
            set(bad FALSE)
        endif ()
        if (bad)
            file(RELATIVE_PATH name "${SOURCE_DIR}" "${source}")
            string(APPEND found "  ${name}: ${line}\n")
        endif ()
    endforeach ()
endforeach ()
if (found)
    message(FATAL_ERROR "bindings/ may include only <omnicore/och.h> from OmniCore Host:\n${found}")
endif ()
