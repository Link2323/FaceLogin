# Development-only liveness fast-fail timing state machine test.
add_executable(LivenessTimingTest
    liveness_timing_test.cpp
)

target_include_directories(LivenessTimingTest PRIVATE
    ${CMAKE_SOURCE_DIR}/vision
)
