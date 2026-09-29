# libhttp: the reusable HTTP/WebSocket server library (reactor + router +
# JSON + pooled buffers).

cmake_minimum_required(VERSION 3.15)
project(libhttp LANGUAGES CXX)

# Standalone builds need CTest for the tests below; a parent project provides its own.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    include(CTest)
endif()

option(HTTP_BUILD_EXAMPLES "Build the libhttp example program" ON)
option(HTTP_BUILD_TESTS "Build CPU-only HTTP tests" ON)
option(HTTP_BUILD_BENCHMARKS "Build the libhttp load-test servers (benchmarks/)" OFF)
# Sanitizers for the library and everything linked with it, e.g.
#   -DHTTP_SANITIZE=address,undefined   or   -DHTTP_SANITIZE=thread
# (the value goes to -fsanitize=; address and thread cannot be combined).
set(HTTP_SANITIZE "" CACHE STRING "Sanitizers to build with (-fsanitize= value), empty for none")
# Fuzz targets (fuzz/): libFuzzer executables with Clang, otherwise a small
# mutation driver that runs a fixed number of iterations (a smoke test under
# ctest, and a fuzzer of sorts under -DHTTP_SANITIZE=address,undefined).
option(HTTP_BUILD_FUZZ "Build the fuzz targets (fuzz/)" OFF)
# Link-time optimization for the library and the programs built here. On the
# request path it measured within run-to-run noise of a plain -O3 build
# (user space is about 15% of a saturated core; the rest is the kernel), so
# it is off by default and here for consumers whose own code may gain from
# it. Silently skipped when the toolchain cannot do it.
option(HTTP_LTO "Build with link-time optimization when the toolchain supports it" OFF)
# Controls TLS support only -- WebSocket's handshake digest is a
# self-contained SHA-1 (src/websocket.cpp) and never needed OpenSSL. Default
# OFF to keep the base build dependency-free (no libssl-dev needed); turn it
# ON to get TLS. OFF makes TlsStream a plain passthrough (src/tls_stream.cpp)
# and configuring ServerConfig::tls throws at runtime instead of failing to
# link.
option(HTTP_WITH_SSL "Build with OpenSSL/TLS support" OFF)
# Linux only: drive sockets through io_uring (liburing, kernel 6.x) instead
# of epoll. Same IoLoop API and behaviour; all of a round's reads and writes
# go to the kernel in one syscall (see src/platform/linux/io_loop_uring.cpp).
option(HTTP_WITH_IO_URING "Use io_uring instead of epoll on Linux (needs liburing)" OFF)

set(HTTP_SOURCES
    src/platform.cpp
    src/memory_pool.cpp
    src/logger.cpp
    src/http_parser.cpp
    src/http_context.cpp
    src/parameters.cpp
    src/tls_stream.cpp
    src/route_pattern.cpp
    src/static_files.cpp
    src/base64.cpp
    src/websocket.cpp
    src/thread_pool.cpp
    src/server.cpp
    src/json.cpp
)

if(WIN32)
    list(APPEND HTTP_SOURCES src/platform/windows/io_loop_iocp.cpp)
elseif(HTTP_WITH_IO_URING)
    list(APPEND HTTP_SOURCES src/platform/linux/io_loop_uring.cpp)
else()
    list(APPEND HTTP_SOURCES src/platform/linux/io_loop_epoll.cpp)
endif()

add_library(http STATIC ${HTTP_SOURCES})

if(NOT WIN32 AND HTTP_WITH_IO_URING)
    # liburing (apt install liburing-dev, or a build of
    # https://github.com/axboe/liburing found through CMAKE_PREFIX_PATH).
    find_path(LIBURING_INCLUDE_DIR liburing.h)
    # The archive first: the example links -static, and liburing has no
    # dependencies of its own to pull in dynamically.
    find_library(LIBURING_LIBRARY NAMES liburing.a uring)
    if(NOT LIBURING_INCLUDE_DIR OR NOT LIBURING_LIBRARY)
        message(FATAL_ERROR "HTTP_WITH_IO_URING=ON but liburing was not found; install liburing-dev or point CMAKE_PREFIX_PATH at a liburing build")
    endif()
    target_include_directories(http SYSTEM PUBLIC ${LIBURING_INCLUDE_DIR})
    target_link_libraries(http PUBLIC ${LIBURING_LIBRARY})
    target_compile_definitions(http PUBLIC HTTP_WITH_IO_URING=1)
endif()

target_include_directories(http PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(http PUBLIC cxx_std_17)
# The optimization level follows CMAKE_BUILD_TYPE (Release: -O3); a build
# configured without one -- easy to do by accident -- still gets -O2 rather
# than nothing.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(HTTP_DEFAULT_OPTIMIZATION $<$<CXX_COMPILER_ID:GNU,Clang>:-O2>)
endif()
target_compile_options(http PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
    ${HTTP_DEFAULT_OPTIMIZATION}
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

if(HTTP_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT HTTP_LTO_SUPPORTED OUTPUT HTTP_LTO_MESSAGE LANGUAGES CXX)
    if(HTTP_LTO_SUPPORTED)
        set_property(TARGET http PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(STATUS "libhttp: link-time optimization not available: ${HTTP_LTO_MESSAGE}")
    endif()
endif()
# Applies HTTP_LTO to a program built here; static-archive LTO only pays
# off when the final link participates too.
function(http_apply_lto target)
    if(HTTP_LTO AND HTTP_LTO_SUPPORTED)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()

find_package(Threads REQUIRED)
target_link_libraries(http PUBLIC Threads::Threads)

if(HTTP_SANITIZE)
    string(REPLACE ";" "," HTTP_SANITIZE_LIST "${HTTP_SANITIZE}")
    target_compile_options(http PUBLIC -fsanitize=${HTTP_SANITIZE_LIST} -fno-omit-frame-pointer)
    target_link_options(http PUBLIC -fsanitize=${HTTP_SANITIZE_LIST})
endif()

if(HTTP_WITH_SSL)
    # OpenSSL provides TLS.
    #
    # Linked by plain name plus an explicit -L rather than through the
    # OpenSSL::Crypto imported target: that target carries an absolute path to
    # libcrypto.so, which makes a -static consumer (http_example below) fail with
    # "attempted static link of dynamic object". By name, the linker picks the .a
    # or the .so according to the consumer's own link mode.
    find_package(OpenSSL 1.1.1 REQUIRED)
    get_filename_component(HTTP_OPENSSL_LIBDIR "${OPENSSL_CRYPTO_LIBRARY}" DIRECTORY)

    target_include_directories(http SYSTEM PUBLIC ${OPENSSL_INCLUDE_DIR})
    target_link_directories(http PUBLIC ${HTTP_OPENSSL_LIBDIR})
    target_link_libraries(http PUBLIC ssl crypto)
    target_compile_definitions(http PUBLIC HTTP_WITH_SSL=1)
endif()

if(HTTP_BUILD_FUZZ)
    foreach(target http_parser ws_frames json)
        add_executable(fuzz_${target} fuzz/fuzz_${target}.cpp)
        target_link_libraries(fuzz_${target} PRIVATE http)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined)
            target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined)
        else()
            target_sources(fuzz_${target} PRIVATE fuzz/fuzz_driver.cpp)
            if(BUILD_TESTING)
                add_test(NAME http_fuzz_${target} COMMAND fuzz_${target} --iterations=3000 --seed=1)
                set_tests_properties(http_fuzz_${target} PROPERTIES TIMEOUT 300)
            endif()
        endif()
    endforeach()
endif()

if(HTTP_BUILD_TESTS AND BUILD_TESTING)
    add_executable(http_test_parameters tests/test_parameters.cpp)
    target_link_libraries(http_test_parameters PRIVATE http)
    add_test(NAME http_parameters COMMAND http_test_parameters)

    add_executable(http_test_memory_pool tests/test_memory_pool.cpp)
    target_link_libraries(http_test_memory_pool PRIVATE http)
    add_test(NAME http_memory_pool COMMAND http_test_memory_pool)
    add_executable(http_test_parser tests/test_parser.cpp)
    target_link_libraries(http_test_parser PRIVATE http)
    add_test(NAME http_parser COMMAND http_test_parser)
    add_executable(http_test_thread_pool tests/test_thread_pool.cpp)
    target_link_libraries(http_test_thread_pool PRIVATE http)
    add_test(NAME http_thread_pool COMMAND http_test_thread_pool)
    add_executable(http_test_task_queue tests/test_task_queue.cpp)
    target_link_libraries(http_test_task_queue PRIVATE http)
    add_test(NAME http_task_queue COMMAND http_test_task_queue)
    add_executable(http_test_timer_queue tests/test_timer_queue.cpp)
    target_link_libraries(http_test_timer_queue PRIVATE http)
    add_test(NAME http_timer_queue COMMAND http_test_timer_queue)
    add_executable(http_test_websocket tests/test_websocket.cpp)
    target_link_libraries(http_test_websocket PRIVATE http)
    add_test(NAME http_websocket COMMAND http_test_websocket)

    add_executable(http_test_json tests/test_json.cpp)
    target_link_libraries(http_test_json PRIVATE http)
    add_test(NAME http_json COMMAND http_test_json)
    add_executable(http_test_json_soak tests/test_json_soak.cpp)
    target_link_libraries(http_test_json_soak PRIVATE http)
    add_test(NAME http_json_soak COMMAND http_test_json_soak)
    set_tests_properties(http_json_soak PROPERTIES TIMEOUT 300)
    if(NOT WIN32)
        add_executable(http_test_logger tests/test_logger.cpp)
        target_link_libraries(http_test_logger PRIVATE http)
        add_test(NAME http_logger COMMAND http_test_logger)
        set_tests_properties(http_logger PROPERTIES TIMEOUT 30)
    endif()

    if(NOT WIN32 AND HTTP_WITH_SSL)
        add_executable(http_test_tls_stream tests/test_tls_stream.cpp)
        target_link_libraries(http_test_tls_stream PRIVATE http)
        add_test(NAME http_tls_stream COMMAND http_test_tls_stream)
        set_tests_properties(http_tls_stream PROPERTIES TIMEOUT 20)
    endif()
    if(NOT WIN32)
        add_executable(http_test_server tests/test_server.cpp)
        target_link_libraries(http_test_server PRIVATE http)
        add_test(NAME http_server COMMAND http_test_server)
        set_tests_properties(http_server PROPERTIES TIMEOUT 15)
        add_executable(http_test_static_files tests/test_static_files.cpp)
        target_link_libraries(http_test_static_files PRIVATE http)
        add_test(NAME http_static_files COMMAND http_test_static_files)
        add_executable(http_test_streaming tests/test_streaming.cpp)
        target_link_libraries(http_test_streaming PRIVATE http)
        add_test(NAME http_streaming COMMAND http_test_streaming)
        set_tests_properties(http_streaming PROPERTIES TIMEOUT 15)
        add_executable(http_test_pipelining tests/test_pipelining.cpp)
        target_link_libraries(http_test_pipelining PRIVATE http)
        add_test(NAME http_pipelining COMMAND http_test_pipelining)
        set_tests_properties(http_pipelining PROPERTIES TIMEOUT 15)
        add_executable(http_test_pool_admission tests/test_pool_admission.cpp)
        target_link_libraries(http_test_pool_admission PRIVATE http)
        add_test(NAME http_pool_admission COMMAND http_test_pool_admission)
        set_tests_properties(http_pool_admission PROPERTIES TIMEOUT 30)
        add_executable(http_test_origin_host tests/test_origin_host.cpp)
        target_link_libraries(http_test_origin_host PRIVATE http)
        add_test(NAME http_origin_host COMMAND http_test_origin_host)
        set_tests_properties(http_origin_host PROPERTIES TIMEOUT 30)
        add_executable(http_test_zombie_session tests/test_zombie_session.cpp)
        target_link_libraries(http_test_zombie_session PRIVATE http)
        add_test(NAME http_zombie_session COMMAND http_test_zombie_session)
        set_tests_properties(http_zombie_session PROPERTIES TIMEOUT 30)
        add_executable(http_test_keepalive tests/test_keepalive.cpp)
        target_link_libraries(http_test_keepalive PRIVATE http)
        add_test(NAME http_keepalive COMMAND http_test_keepalive)
        set_tests_properties(http_keepalive PROPERTIES TIMEOUT 30)
        add_executable(http_test_limits tests/test_limits.cpp)
        target_link_libraries(http_test_limits PRIVATE http)
        add_test(NAME http_limits COMMAND http_test_limits)
        set_tests_properties(http_limits PROPERTIES TIMEOUT 60)
        add_executable(http_test_response_headers tests/test_response_headers.cpp)
        target_link_libraries(http_test_response_headers PRIVATE http)
        add_test(NAME http_response_headers COMMAND http_test_response_headers)
        set_tests_properties(http_response_headers PROPERTIES TIMEOUT 30)
        add_executable(http_test_server_stats tests/test_server_stats.cpp)
        target_link_libraries(http_test_server_stats PRIVATE http)
        add_test(NAME http_server_stats COMMAND http_test_server_stats)
        set_tests_properties(http_server_stats PROPERTIES TIMEOUT 30)
        add_executable(http_test_listen_addresses tests/test_listen_addresses.cpp)
        target_link_libraries(http_test_listen_addresses PRIVATE http)
        add_test(NAME http_listen_addresses COMMAND http_test_listen_addresses)
        set_tests_properties(http_listen_addresses PROPERTIES TIMEOUT 60)
        add_executable(http_test_error_status tests/test_error_status.cpp)
        target_link_libraries(http_test_error_status PRIVATE http)
        add_test(NAME http_error_status COMMAND http_test_error_status)
        set_tests_properties(http_error_status PROPERTIES TIMEOUT 30)
        add_executable(http_test_callback_release tests/test_callback_release.cpp)
        target_link_libraries(http_test_callback_release PRIVATE http)
        add_test(NAME http_callback_release COMMAND http_test_callback_release)
        set_tests_properties(http_callback_release PROPERTIES TIMEOUT 30)
    endif()
endif()

if(WIN32)
    target_link_libraries(http PUBLIC ws2_32 mswsock)
endif()

if(HTTP_BUILD_BENCHMARKS)
    add_executable(http_bench_hello benchmarks/hello.cpp)
    target_link_libraries(http_bench_hello PRIVATE http)
    http_apply_lto(http_bench_hello)
    # Kernel diagnostics for the io_uring backend (liburing only, no libhttp).
    if(NOT WIN32 AND HTTP_WITH_IO_URING)
        add_executable(http_io_uring_probe benchmarks/io_uring_probe.cpp)
        target_include_directories(http_io_uring_probe SYSTEM PRIVATE ${LIBURING_INCLUDE_DIR})
        target_link_libraries(http_io_uring_probe PRIVATE ${LIBURING_LIBRARY} Threads::Threads)
    endif()
endif()

if(HTTP_BUILD_EXAMPLES)
    add_executable(http_example examples/quickstart.cpp)
    target_link_libraries(http_example PRIVATE http)
    target_compile_options(http_example PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
        ${HTTP_DEFAULT_OPTIMIZATION}
    )
    http_apply_lto(http_example)

    # Fully static -- no libstdc++/libgcc/libc/libcrypto .so dependencies at
    # runtime, so it drops onto a target device without a matching toolchain
    # (e.g. a bare Jetson image). Needs libssl-dev's static archives.
    #
    # glibc before 2.34 (Ubuntu 20.04 / JetPack 5) keeps pthread and dl in
    # archives of their own, and libcrypto.a's references to them
    # (rwlocks, dlopen) come too late on the line for the linker's single
    # pass. Pulling both archives in whole up front resolves that; on a glibc
    # that already folded them into libc the archives are stubs and it is a
    # no-op.
    if(NOT WIN32 AND HTTP_WITH_SSL AND EXISTS "${HTTP_OPENSSL_LIBDIR}/libcrypto.a" AND EXISTS "${HTTP_OPENSSL_LIBDIR}/libssl.a")
        target_link_options(http_example PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-static>
            $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<PLATFORM_ID:Linux>>:-Wl,--whole-archive;-lpthread;-ldl;-Wl,--no-whole-archive>
        )
    endif()
endif()
