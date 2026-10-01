cmake_minimum_required(VERSION 3.13)

project(ds5dongle-host-tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(hd_rumble_decode_test
        hd_rumble_decode_test.cpp
        ../src/switch_hd_rumble.cpp
)
target_include_directories(hd_rumble_decode_test PRIVATE
        ../src
)

enable_testing()
add_test(NAME hd_rumble_decode COMMAND hd_rumble_decode_test)
