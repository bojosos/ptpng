cmake_minimum_required(VERSION 3.15)
project(ptpng C)

option(PTPNG_WITH_LIBPNG "Build the vendored zlib+libpng reference and the parity/benchmark tools" ON)
option(PTPNG_BUILD_BENCH "Build bench_compare (nightly perf tracking)" ON)
option(PTPNG_WITH_ZLIB_NG "Also build libpng+zlib-ng reference for benchmarks" ON)
option(PTPNG_BUILD_FUZZERS "Build Clang libFuzzer targets with ASan and UBSan" OFF)
option(PTPNG_BUILD_CPP_TESTS "Build C++ wrapper tests; requires a C++17 compiler" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Keep CLI paths identical for single- and multi-configuration generators.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<0:>")

# ---- the library -------------------------------------------------------
set(PTPNG_SOURCES src/ptpng.c src/ptpng_inflate.c src/ptpng_crc.c
                  src/ptpng_filters.c src/ptpng_encode.c src/ptpng_deflate.c)

set(PTPNG_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
if(CMAKE_GENERATOR_PLATFORM)
    set(PTPNG_PROCESSOR "${CMAKE_GENERATOR_PLATFORM}")
elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(PTPNG_PROCESSOR "${CMAKE_OSX_ARCHITECTURES}")
endif()
string(TOLOWER "${PTPNG_PROCESSOR}" PTPNG_PROCESSOR)
if(PTPNG_PROCESSOR MATCHES "^(x86_64|amd64|x64|i.86|x86|win32)$")
    set(PTPNG_X86 ON)
elseif(PTPNG_PROCESSOR MATCHES "^(aarch64|arm64|arm.*)$")
    set(PTPNG_ARM ON)
endif()

add_library(ptpng STATIC ${PTPNG_SOURCES})
add_library(ptpng::ptpng ALIAS ptpng)
target_include_directories(ptpng PUBLIC include PRIVATE src)

if(PTPNG_X86)
    # AVX2 translation unit: compiled with its own ISA flag; all code in
    # it is behind runtime dispatch (ptpng_avx2_init checks CPUID).
    if(MSVC)
        set_source_files_properties(src/ptpng_avx2.c src/ptpng_encode_avx2.c PROPERTIES
                                    COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(src/ptpng_avx2.c src/ptpng_encode_avx2.c PROPERTIES
                                    COMPILE_OPTIONS "-mavx2")
    endif()
    if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "Clang")
        set_source_files_properties(src/ptpng_crc_x86.c PROPERTIES
                                    COMPILE_OPTIONS "/clang:-mpclmul;/clang:-msse2")
    elseif(NOT MSVC)
        set_source_files_properties(src/ptpng_crc_x86.c PROPERTIES
                                    COMPILE_OPTIONS "-mpclmul;-msse2")
    endif()
    target_sources(ptpng PRIVATE src/ptpng_avx2.c src/ptpng_encode_avx2.c
                                 src/ptpng_crc_x86.c)
elseif(PTPNG_ARM)
    # NEON is baseline on aarch64; no extra flags needed. ptpng_neon.c
    # compiles to an empty TU if NEON is unavailable (armv7 without it).
    target_sources(ptpng PRIVATE src/ptpng_neon.c src/ptpng_encode_neon.c
                                 src/ptpng_crc_arm.c)
endif()

target_sources(ptpng PRIVATE src/ptpng_inflate_fixed.c)

if(MSVC)
    target_compile_options(ptpng PRIVATE /W3
                                         /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(ptpng PRIVATE -Wall -Wextra
                              -Wno-unused-parameter -fno-strict-aliasing)
endif()

# ---- the CLI tool and unit testers -------------------------------------
set(PTPNG_TOOLS ptpng_tool filters_test sums_test inflate_test inf_bench fuzz_png decoder_test encoder_test encoder_filters_test deflate_test profile_png)
foreach(tgt ${PTPNG_TOOLS})
    add_executable(${tgt} tools/${tgt}.c)
    target_link_libraries(${tgt} PRIVATE ptpng)
endforeach()
foreach(tgt filters_test sums_test inflate_test inf_bench decoder_test
            encoder_filters_test deflate_test)
    target_include_directories(${tgt} PRIVATE src)
endforeach()
if(MSVC)
    foreach(tgt ${PTPNG_TOOLS})
        target_compile_options(${tgt} PRIVATE
                               /W3 /D_CRT_SECURE_NO_WARNINGS)
    endforeach()
else()
    foreach(tgt ${PTPNG_TOOLS})
        target_compile_options(${tgt} PRIVATE -Wall -Wextra)
    endforeach()
endif()

enable_testing()
add_test(NAME unit COMMAND filters_test)
add_test(NAME sums COMMAND sums_test)
add_test(NAME decoder COMMAND decoder_test)
add_test(NAME encoder COMMAND encoder_test)
add_test(NAME encoder_filters COMMAND encoder_filters_test)
add_test(NAME deflate COMMAND deflate_test)

# Header-only wrapper consumers choose their own C++ standard. C-only builds
# never enable CXX or require a C++ compiler.
if(PTPNG_BUILD_CPP_TESTS)
    enable_language(CXX)
    add_executable(cpp_test tools/cpp_test.cpp)
    target_link_libraries(cpp_test PRIVATE ptpng::ptpng)
    target_compile_features(cpp_test PRIVATE cxx_std_17)
    set_target_properties(cpp_test PROPERTIES CXX_EXTENSIONS OFF)
    add_test(NAME cpp COMMAND cpp_test)
    if("cxx_std_20" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        add_executable(cpp20_test tools/cpp_test.cpp)
        target_link_libraries(cpp20_test PRIVATE ptpng::ptpng)
        target_compile_features(cpp20_test PRIVATE cxx_std_20)
        target_compile_definitions(cpp20_test PRIVATE PTPNG_TEST_SPAN)
        set_target_properties(cpp20_test PROPERTIES CXX_EXTENSIONS OFF)
        add_test(NAME cpp20 COMMAND cpp20_test)
    endif()
    foreach(tgt cpp_test cpp20_test)
        if(TARGET ${tgt})
            if(MSVC)
                target_compile_options(${tgt} PRIVATE /W4)
            else()
                target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
            endif()
        endif()
    endforeach()
endif()

if(PTPNG_BUILD_FUZZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR MSVC)
        message(FATAL_ERROR "PTPNG_BUILD_FUZZERS requires Clang with the Unix libFuzzer runtime")
    endif()
    target_compile_options(ptpng PRIVATE -fsanitize=fuzzer-no-link,address,undefined
                                         -fno-omit-frame-pointer)
    target_link_options(ptpng INTERFACE -fsanitize=address,undefined)
    foreach(tgt fuzz_decode fuzz_inflate fuzz_encode fuzz_deflate)
        add_executable(${tgt} tools/${tgt}.c)
        target_link_libraries(${tgt} PRIVATE ptpng)
        target_compile_options(${tgt} PRIVATE -fsanitize=fuzzer,address,undefined
                                              -fno-omit-frame-pointer)
        target_link_options(${tgt} PRIVATE -fsanitize=fuzzer,address,undefined)
    endforeach()
    target_include_directories(fuzz_inflate PRIVATE src)
    target_include_directories(fuzz_deflate PRIVATE src)
endif()

# ---- reference build (zlib + libpng) and parity/bench tools ------------
if(PTPNG_WITH_LIBPNG)
    # Configure reference libraries for the same compiler and target as ptpng.
    set(PTPNG_REFERENCE_ARGS -G "${CMAKE_GENERATOR}"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=lib)
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND PTPNG_REFERENCE_ARGS -A "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND PTPNG_REFERENCE_ARGS -T "${CMAKE_GENERATOR_TOOLSET}")
    endif()
    foreach(var CMAKE_TOOLCHAIN_FILE CMAKE_OSX_ARCHITECTURES
                CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT
                CMAKE_C_COMPILER_TARGET CMAKE_SYSROOT CMAKE_MSVC_RUNTIME_LIBRARY)
        if(DEFINED ${var})
            list(APPEND PTPNG_REFERENCE_ARGS "-D${var}=${${var}}")
        endif()
    endforeach()
    set(PTPNG_ZLIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib-develop)
    set(PTPNG_LIBPNG_DIR
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libpng-libpng16)
    set(ZBUILD ${CMAKE_BINARY_DIR}/zlibb)

    # build+install the zlib static library at configure time so that
    # libpng's find_package(ZLIB) can see it through the cache vars
    file(MAKE_DIRECTORY ${ZBUILD})
    execute_process(
        COMMAND ${CMAKE_COMMAND} ${PTPNG_ZLIB_DIR}
                ${PTPNG_REFERENCE_ARGS}
                -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON
                -DZLIB_BUILD_TESTING=OFF
                -DCMAKE_INSTALL_PREFIX=${ZBUILD}/inst
        WORKING_DIRECTORY ${ZBUILD} RESULT_VARIABLE rz)
    if(rz EQUAL 0)
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build ${ZBUILD} --config Release
                    --target install
            WORKING_DIRECTORY ${ZBUILD} RESULT_VARIABLE rz)
    endif()
    if(NOT rz EQUAL 0)
        message(FATAL_ERROR "zlib reference build failed")
    endif()

    # locate the static zlib library regardless of generator naming:
    # prefer explicitly-static artifacts (libzs.lib / *static*.lib /
    # libz.a); plain libz.lib can be the DLL import library on Windows
    file(GLOB PTPNG_ZLIB_ALL "${ZBUILD}/inst/lib/*")
    set(PTPNG_ZLIB_LIB "")
    foreach(cand ${PTPNG_ZLIB_ALL})
        get_filename_component(nm "${cand}" NAME)
        if(nm MATCHES "static|libzs|^libz\\.a$|^z\\.a$")
            set(PTPNG_ZLIB_LIB "${cand}")
        endif()
    endforeach()
    if(NOT PTPNG_ZLIB_LIB)
        foreach(cand ${PTPNG_ZLIB_ALL})
            get_filename_component(nm "${cand}" NAME)
            if(nm STREQUAL "libz.lib" OR nm STREQUAL "zlibstatic.lib")
                set(PTPNG_ZLIB_LIB "${cand}")
            endif()
        endforeach()
    endif()
    if(NOT PTPNG_ZLIB_LIB)
        message(FATAL_ERROR "zlib static library not found in ${ZBUILD}/inst/lib")
    endif()
    message(STATUS "reference zlib: ${PTPNG_ZLIB_LIB}")

    set(ZLIB_INCLUDE_DIR "${ZBUILD}/inst/include" CACHE PATH "" FORCE)
    set(ZLIB_LIBRARY "${PTPNG_ZLIB_LIB}" CACHE FILEPATH "" FORCE)
    set(PNG_SHARED OFF CACHE BOOL "" FORCE)
    set(PNG_STATIC ON CACHE BOOL "" FORCE)
    set(PNG_TESTS OFF CACHE BOOL "" FORCE)
    set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
    set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
    add_subdirectory(${PTPNG_LIBPNG_DIR} libpngb)
    target_compile_definitions(encoder_test PRIVATE PTPNG_TEST_LIBPNG)
    target_link_libraries(encoder_test PRIVATE png_static)
    target_compile_definitions(deflate_test PRIVATE PTPNG_TEST_ZLIB)
    target_include_directories(deflate_test PRIVATE ${ZBUILD}/inst/include)
    target_link_libraries(deflate_test PRIVATE ${PTPNG_ZLIB_LIB})
    if(PTPNG_BUILD_FUZZERS)
        target_compile_definitions(fuzz_decode PRIVATE PTPNG_FUZZ_LIBPNG)
        target_link_libraries(fuzz_decode PRIVATE png_static)
        target_compile_definitions(fuzz_encode PRIVATE PTPNG_FUZZ_LIBPNG)
        target_link_libraries(fuzz_encode PRIVATE png_static)
        foreach(tgt fuzz_inflate fuzz_deflate)
            target_compile_definitions(${tgt} PRIVATE PTPNG_FUZZ_ZLIB)
            target_include_directories(${tgt} PRIVATE ${ZBUILD}/inst/include)
            target_link_libraries(${tgt} PRIVATE ${PTPNG_ZLIB_LIB})
        endforeach()
    endif()

    # All targets inherit the same MSVC runtime selection.
    set(PTPNG_REF_TOOLS verify_suite)
    if(PTPNG_BUILD_BENCH)
        list(APPEND PTPNG_REF_TOOLS bench_compare bench_encode)
    endif()
    foreach(tgt ${PTPNG_REF_TOOLS})
        add_executable(${tgt} tools/${tgt}.c)
        target_include_directories(${tgt} PRIVATE
                                   ${PTPNG_LIBPNG_DIR} ${ZBUILD})
        target_link_libraries(${tgt} PRIVATE ptpng png_static)
        if(MSVC)
            target_compile_options(${tgt} PRIVATE
                                   /W3 /D_CRT_SECURE_NO_WARNINGS)
        else()
            target_compile_options(${tgt} PRIVATE -Wall -Wextra)
        endif()
    endforeach()
    if(TARGET bench_encode)
        target_include_directories(bench_encode PRIVATE src ${ZBUILD}/inst/include)
    endif()

    add_test(NAME parity COMMAND verify_suite
             ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libpng-libpng16/contrib/testpngs
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/gen)

    if(WIN32)
        add_executable(libpng_bench tools/libpng_bench.c)
        target_include_directories(libpng_bench PRIVATE
                                   ${PTPNG_LIBPNG_DIR} ${ZBUILD})
        target_link_libraries(libpng_bench PRIVATE png_static)
        if(MSVC)
            target_compile_options(libpng_bench PRIVATE
                                   /W3 /D_CRT_SECURE_NO_WARNINGS)
        endif()
    endif()

    # ---- libpng + zlib-ng reference (benchmark opponent) --------------
    # zlib-ng in ZLIB_COMPAT mode exposes the same symbols as zlib, so
    # it cannot coexist with the stock-zlib libpng in one binary.  It
    # gets its own configure-time build tree and its own bench binary.
    if(PTPNG_WITH_ZLIB_NG AND PTPNG_BUILD_BENCH)
        set(ZNG ${CMAKE_BINARY_DIR}/zngb)
        set(PTPNG_ZNG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib-ng-2.2.4)
        file(MAKE_DIRECTORY ${ZNG})
        execute_process(
            COMMAND ${CMAKE_COMMAND} ${PTPNG_ZNG_DIR}
                    ${PTPNG_REFERENCE_ARGS}
                    -DZLIB_COMPAT=ON -DBUILD_SHARED_LIBS=OFF
                    -DZLIB_ENABLE_TESTS=OFF -DWITH_GTEST=OFF
                    -DWITH_FUZZERS=OFF -DWITH_BENCHMARKS=OFF
                    -DWITH_MAINTAINER_WARNINGS=OFF -DWITH_SANITIZER=OFF
                    -DCMAKE_BUILD_TYPE=Release
                    -DCMAKE_INSTALL_PREFIX=${ZNG}/inst
            WORKING_DIRECTORY ${ZNG} RESULT_VARIABLE rz2)
        if(rz2 EQUAL 0)
            execute_process(
                COMMAND ${CMAKE_COMMAND} --build ${ZNG} --config Release
                        --target install
                WORKING_DIRECTORY ${ZNG} RESULT_VARIABLE rz2)
        endif()
        if(NOT rz2 EQUAL 0)
            message(FATAL_ERROR "zlib-ng reference build failed")
        else()
            file(GLOB PTPNG_ZNG_ALL "${ZNG}/inst/lib/*")
            set(PTPNG_ZNG_LIB "")
            foreach(cand ${PTPNG_ZNG_ALL})
                get_filename_component(nm "${cand}" NAME)
                if(nm MATCHES "^(zlibstatic\\.lib|libzs\\.lib|zlib\\.lib|libz\\.a)$")
                    set(PTPNG_ZNG_LIB "${cand}")
                endif()
            endforeach()
            if(NOT PTPNG_ZNG_LIB)
                message(FATAL_ERROR "zlib-ng static library not found")
            else()
                message(STATUS "reference zlib-ng: ${PTPNG_ZNG_LIB}")

                # second libpng, built at configure time against zlib-ng
                set(PNGB ${CMAKE_BINARY_DIR}/libpngng)
                file(MAKE_DIRECTORY ${PNGB})
                execute_process(
                    COMMAND ${CMAKE_COMMAND} ${PTPNG_LIBPNG_DIR}
                            ${PTPNG_REFERENCE_ARGS}
                            -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF
                            -DPNG_TOOLS=OFF -DPNG_FRAMEWORK=OFF
                            -DCMAKE_BUILD_TYPE=Release
                            -DZLIB_INCLUDE_DIR=${ZNG}/inst/include
                            -DZLIB_LIBRARY=${PTPNG_ZNG_LIB}
                    WORKING_DIRECTORY ${PNGB} RESULT_VARIABLE rp2)
                if(rp2 EQUAL 0)
                    execute_process(
                        COMMAND ${CMAKE_COMMAND} --build ${PNGB}
                                --config Release --target png_static
                        WORKING_DIRECTORY ${PNGB} RESULT_VARIABLE rp2)
                endif()
                if(NOT rp2 EQUAL 0)
                    message(FATAL_ERROR "libpng+zlib-ng build failed")
                else()
                    # locate the built static libpng (multi- and single-config)
                    file(GLOB PNGNG_CAND "${PNGB}/*.lib" "${PNGB}/Release/*.lib"
                         "${PNGB}/libpng.a" "${PNGB}/libpng*.a")
                    set(PNGNG_LIB "")
                    foreach(cand ${PNGNG_CAND})
                        get_filename_component(nm "${cand}" NAME)
                        if(nm MATCHES "png")
                            set(PNGNG_LIB "${cand}")
                        endif()
                    endforeach()
                    if(NOT PNGNG_LIB)
                        message(FATAL_ERROR "libpng(zng) static lib not found")
                    else()
                        message(STATUS "reference libpng(zlib-ng): ${PNGNG_LIB}")
                        add_executable(bench_zlibng tools/bench_compare.c)
                        target_compile_definitions(bench_zlibng PRIVATE
                                                   REF_LABEL="libpng-zng")
                        target_include_directories(bench_zlibng PRIVATE
                                   ${PTPNG_LIBPNG_DIR} ${ZNG}/inst/include ${PNGB})
                        target_link_libraries(bench_zlibng PRIVATE
                                              ptpng ${PNGNG_LIB}
                                              ${PTPNG_ZNG_LIB})
                        if(UNIX)
                            target_link_libraries(bench_zlibng PRIVATE m)
                        endif()
                        if(MSVC)
                            target_compile_options(bench_zlibng PRIVATE
                                   /W3 /D_CRT_SECURE_NO_WARNINGS)
                        else()
                            target_compile_options(bench_zlibng PRIVATE -Wall -Wextra)
                        endif()
                        add_executable(bench_encode_zlibng tools/bench_encode.c)
                        target_include_directories(bench_encode_zlibng PRIVATE src)
                        target_compile_definitions(bench_encode_zlibng PRIVATE REF_LABEL="libpng-zng")
                        target_include_directories(bench_encode_zlibng PRIVATE
                            ${PTPNG_LIBPNG_DIR} ${ZNG}/inst/include ${PNGB})
                        target_link_libraries(bench_encode_zlibng PRIVATE
                            ptpng ${PNGNG_LIB} ${PTPNG_ZNG_LIB})
                        if(UNIX)
                            target_link_libraries(bench_encode_zlibng PRIVATE m)
                        endif()
                        if(MSVC)
                            target_compile_options(bench_encode_zlibng PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS)
                        else()
                            target_compile_options(bench_encode_zlibng PRIVATE -Wall -Wextra)
                        endif()
                    endif()
                endif()
            endif()
        endif()
    endif()
endif()
