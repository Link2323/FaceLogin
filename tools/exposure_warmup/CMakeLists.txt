# Development-only exposure warmup state machine test (+ optional --camera
# live DirectShow probe). This target is never shipped.
add_executable(ExposureWarmupTest
    exposure_warmup_test.cpp
    ${CMAKE_SOURCE_DIR}/face_service/webcam_capture_dshow.cpp
)

target_link_libraries(ExposureWarmupTest PRIVATE
    facelogin_common
    strmiids
    advapi32
    ole32
)

target_include_directories(ExposureWarmupTest PRIVATE
    ${CMAKE_SOURCE_DIR}/face_service
)

set_target_properties(ExposureWarmupTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
