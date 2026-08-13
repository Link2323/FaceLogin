# Development-only DirectShow lifecycle probe. This target is never shipped.
add_executable(CameraLifecycleTest
    camera_lifecycle.cpp
    ${CMAKE_SOURCE_DIR}/face_service/camera_backend.cpp
    ${CMAKE_SOURCE_DIR}/face_service/webcam_capture.cpp
    ${CMAKE_SOURCE_DIR}/face_service/webcam_capture_dshow.cpp
)

target_link_libraries(CameraLifecycleTest PRIVATE
    facelogin_common
    strmiids
    advapi32
    ole32
    psapi
    mfplat
    mf
    mfreadwrite
    mfuuid
)

target_include_directories(CameraLifecycleTest PRIVATE
    ${CMAKE_SOURCE_DIR}/face_service
)

set_target_properties(CameraLifecycleTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
