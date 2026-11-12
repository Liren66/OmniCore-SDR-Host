# Header-only helpers shared by the bindings (SoapySDR module, MATLAB MEX gateway): channel
# (de)interleaving, value mappings, RX block verdicts and TX burst bookkeeping. They need only the
# public header <omnicore/och.h>, never the library itself. Sources include them as "common/X.hpp".
#
# Added by the top-level project, and by each binding when it is built on its own against an
# installed OmniCore Host; the first one to add it defines the target and the tests.
if (TARGET och_bindings_common)
    return()
endif ()

add_library(och_bindings_common INTERFACE)
target_include_directories(och_bindings_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..
        $<TARGET_PROPERTY:OmniCore::omnicore,INTERFACE_INCLUDE_DIRECTORIES>)

# Offline tests of the pure logic: no device, SoapySDR or MATLAB needed.
add_executable(bindings_mapping_test tests/mapping_test.cpp)
target_link_libraries(bindings_mapping_test PRIVATE och_bindings_common)
add_test(NAME bindings_mapping COMMAND bindings_mapping_test)

add_executable(bindings_interleave_test tests/interleave_test.cpp)
target_link_libraries(bindings_interleave_test PRIVATE och_bindings_common)
add_test(NAME bindings_interleave COMMAND bindings_interleave_test)
