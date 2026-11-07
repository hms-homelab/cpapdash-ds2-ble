cmake_minimum_required(VERSION 3.16)
project(cpapdash_ds2_ble VERSION 0.1.0 LANGUAGES CXX)

# Reading a Philips DreamStation 2 over Bluetooth LE: the frames, the commands
# and the record walk. No radio code: a caller hands in the bytes the machine
# notified and sends the bytes it is given. Consumed with FetchContent, pinned
# by tag.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(cpapdash_ds2_ble STATIC
    src/Frame.cpp
    src/ReadFile.cpp
    src/Record.cpp
    src/SyncSession.cpp
)
add_library(cpapdash::ds2_ble ALIAS cpapdash_ds2_ble)

target_include_directories(cpapdash_ds2_ble PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(cpapdash_ds2_ble PRIVATE /W4)
else()
    target_compile_options(cpapdash_ds2_ble PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The tests and the replay tool are built only for this project itself, never
# when a consumer pulls the library in with FetchContent.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_ds2ble_top ON)
else()
    set(_ds2ble_top OFF)
endif()
option(CPAPDASH_DS2_BLE_BUILD_TESTS "Build the cpapdash-ds2-ble tests" ${_ds2ble_top})
option(CPAPDASH_DS2_BLE_BUILD_TOOLS "Build ds2ble-replay" ${_ds2ble_top})

if(CPAPDASH_DS2_BLE_BUILD_TOOLS)
    add_executable(ds2ble-replay tools/ds2ble_replay.cpp tests/Replay.cpp)
    target_include_directories(ds2ble-replay PRIVATE tests)
    target_link_libraries(ds2ble-replay cpapdash_ds2_ble)
endif()

if(CPAPDASH_DS2_BLE_BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
        add_library(GTest::gtest_main ALIAS gtest_main)
    endif()

    add_executable(ds2ble_tests
        tests/test_frame.cpp
        tests/test_read_file.cpp
        tests/test_record.cpp
        tests/test_sync.cpp
        tests/test_capture.cpp
        tests/Replay.cpp
        tests/FakeMachine.cpp
    )
    target_include_directories(ds2ble_tests PRIVATE tests)
    target_link_libraries(ds2ble_tests cpapdash_ds2_ble GTest::gtest_main)
    add_test(NAME ds2ble_tests COMMAND ds2ble_tests)
endif()
