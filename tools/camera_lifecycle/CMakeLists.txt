# Development-only DirectShow lifecycle probe. This target is never shipped.
add_executable(CameraLifecycleTest
    camera_lifecycle.cpp
)

target_link_libraries(CameraLifecycleTest PRIVATE
    facelogin_camera
    psapi
)

set_target_properties(CameraLifecycleTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

add_executable(ExposureResponseProbe exposure_response_probe.cpp)
target_link_libraries(ExposureResponseProbe PRIVATE facelogin_camera)
set_target_properties(ExposureResponseProbe PROPERTIES
    EXCLUDE_FROM_ALL TRUE
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
