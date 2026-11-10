# OmniCore Host examples.
#
# Built two ways:
#   - with the main project (option OCH_BUILD_EXAMPLES, on by default), or
#   - on their own against an installed OmniCore Host:
#       cmake -S <prefix>/examples -B build -DCMAKE_PREFIX_PATH=<prefix>
#       cmake --build build
cmake_minimum_required(VERSION 3.20)

if(NOT TARGET OmniCore::omnicore)
  project(OmniCoreHostExamples LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  find_package(OmniCoreHost 0.7 CONFIG REQUIRED)
endif()
find_package(Threads REQUIRED)

foreach(name receive transmit reconfigure duplex_iq8)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE OmniCore::omnicore Threads::Threads)
  if(MINGW)
    target_link_options(${name} PRIVATE -static) # like och: no MinGW runtime DLLs needed
  endif()
  # Windows has no rpath: put in-tree examples next to omnicore.dll and libusbK.dll.
  if(WIN32 AND TARGET omnicore)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:omnicore>")
  endif()
endforeach()
