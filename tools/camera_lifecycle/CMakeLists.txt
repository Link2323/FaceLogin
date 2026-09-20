# Development-only DirectShow lifecycle probe. This target is never shipped.
add_executable(CameraLifecycleTest
    camera_lifecycle.cpp
    ${CMAKE_SOURCE_DIR}/face_service/webcam_capture_dshow.cpp
)

target_link_libraries(CameraLifecycleTest PRIVATE
    facelogin_common
    strmiids
    advapi32
    ole32
    psapi
)

target_include_directories(CameraLifecycleTest PRIVATE
    ${CMAKE_SOURCE_DIR}/face_service
)

set_target_properties(CameraLifecycleTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

add_executable(ExposureResponseProbe exposure_response_probe.cpp
    ${CMAKE_SOURCE_DIR}/face_service/webcam_capture_dshow.cpp)
target_include_directories(ExposureResponseProbe PRIVATE ${CMAKE_SOURCE_DIR}/face_service)
target_link_libraries(ExposureResponseProbe PRIVATE facelogin_common strmiids advapi32 ole32)
set_target_properties(ExposureResponseProbe PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
