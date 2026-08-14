# Development-only liveness fast-fail timing state machine test.
add_executable(LivenessTimingTest
    liveness_timing_test.cpp
)

target_include_directories(LivenessTimingTest PRIVATE
    ${CMAKE_SOURCE_DIR}/face_service
)

set_target_properties(LivenessTimingTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
