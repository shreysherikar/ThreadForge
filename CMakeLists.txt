cmake_minimum_required(VERSION 3.16)
project(ThreadForge
    VERSION 1.0.0
    DESCRIPTION "Production-Quality Multithreaded Priority Task Scheduler in Modern C++17"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(THREADFORGE_BUILD_TESTS "Build ThreadForge test suite" ON)
option(THREADFORGE_BUILD_EXAMPLES "Build ThreadForge example applications" ON)
option(THREADFORGE_BUILD_BENCHMARKS "Build ThreadForge benchmark suite" ON)
option(THREADFORGE_ENABLE_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(THREADFORGE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

# Sanitizer Mutual Exclusivity Check
if(THREADFORGE_ENABLE_ASAN_UBSAN AND THREADFORGE_ENABLE_TSAN)
    message(FATAL_ERROR "THREADFORGE_ENABLE_ASAN_UBSAN and THREADFORGE_ENABLE_TSAN are mutually exclusive. Please enable only one.")
endif()

# Threading Library
find_package(Threads REQUIRED)

# Core Library Target
add_library(threadforge
    src/task.cpp
    src/worker_pool.cpp
    src/task_scheduler.cpp
)

target_include_directories(threadforge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(threadforge
    PUBLIC
        Threads::Threads
)

# Portable Compiler Warnings and Compiler Flags
if(MSVC)
    target_compile_options(threadforge PRIVATE /W4 /permissive-)
else()
    target_compile_options(threadforge PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Apply Sanitizers
if(THREADFORGE_ENABLE_ASAN_UBSAN)
    if(MSVC)
        target_compile_options(threadforge PUBLIC /fsanitize=address)
    else()
        target_compile_options(threadforge PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(threadforge PUBLIC -fsanitize=address,undefined)
    endif()
elseif(THREADFORGE_ENABLE_TSAN)
    if(MSVC)
        message(FATAL_ERROR "ThreadSanitizer is not supported on MSVC.")
    else()
        target_compile_options(threadforge PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(threadforge PUBLIC -fsanitize=thread)
    endif()
endif()

# ==============================================================================
# Examples
# ==============================================================================
if(THREADFORGE_BUILD_EXAMPLES)
    set(EXAMPLES
        basic_usage
        priority_scheduling
        concurrent_submission
        exception_handling
        graceful_shutdown
    )

    foreach(EXAMPLE ${EXAMPLES})
        add_executable(example_${EXAMPLE} examples/${EXAMPLE}.cpp)
        target_link_libraries(example_${EXAMPLE} PRIVATE threadforge)
        if(NOT MSVC)
            target_compile_options(example_${EXAMPLE} PRIVATE -Wall -Wextra -Wpedantic)
        else()
            target_compile_options(example_${EXAMPLE} PRIVATE /W4)
        endif()
    endforeach()
endif()

# ==============================================================================
# Benchmarks
# ==============================================================================
if(THREADFORGE_BUILD_BENCHMARKS)
    add_executable(scheduler_benchmark benchmarks/scheduler_benchmark.cpp)
    target_link_libraries(scheduler_benchmark PRIVATE threadforge)
    if(NOT MSVC)
        target_compile_options(scheduler_benchmark PRIVATE -Wall -Wextra -Wpedantic -O3)
    else()
        target_compile_options(scheduler_benchmark PRIVATE /W4 /O2)
    endif()
endif()

# ==============================================================================
# Testing
# ==============================================================================
if(THREADFORGE_BUILD_TESTS)
    enable_testing()

    # Find local GoogleTest or fetch reproducibly if not available
    find_package(GTest QUIET)

    if(NOT GTest_FOUND)
        message(STATUS "GoogleTest not found locally. Fetching GoogleTest v1.14.0...")
        include(FetchContent)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    else()
        message(STATUS "Using system GoogleTest ${GTest_VERSION}")
    endif()

    set(TEST_SUITES
        test_task_scheduler
        test_priority_queue
        test_concurrency
        test_exception_handling
        test_shutdown
        test_task_results
    )

    foreach(TEST_SUITE ${TEST_SUITES})
        add_executable(${TEST_SUITE} tests/${TEST_SUITE}.cpp)
        target_link_libraries(${TEST_SUITE}
            PRIVATE
                threadforge
                GTest::gtest
                GTest::gtest_main
        )
        if(NOT MSVC)
            target_compile_options(${TEST_SUITE} PRIVATE -Wall -Wextra -Wpedantic)
        else()
            target_compile_options(${TEST_SUITE} PRIVATE /W4)
        endif()

        add_test(NAME ${TEST_SUITE} COMMAND ${TEST_SUITE})
        set_tests_properties(${TEST_SUITE} PROPERTIES TIMEOUT 60)
    endforeach()
endif()
