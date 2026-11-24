cmake_minimum_required(VERSION 3.21)
project(XenosagaHostChecks LANGUAGES CXX)
include(CTest)
set(PS2RECOMP "${CMAKE_CURRENT_LIST_DIR}/../../../PS2Recomp" CACHE PATH "Patched PS2Recomp checkout")
add_subdirectory("${PS2RECOMP}/ps2xTest/gs_cache" gs_cache)
# The patched GS frontend uses the presentation hooks even in CPU-only tests.
target_sources(ps2_gs_cache_backend_under_test PRIVATE
    "${PS2RECOMP}/ps2xRuntime/src/lib/gs/gs_gl_backend.cpp")
target_compile_options(ps2_gs_cache_backend_under_test PRIVATE -mavx2)
add_executable(xs_iso_test iso_test.cpp)
target_compile_features(xs_iso_test PRIVATE cxx_std_17)
add_test(NAME port.iso_lookup COMMAND xs_iso_test)
