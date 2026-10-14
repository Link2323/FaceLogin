add_executable(PadCalibration
    main.cpp
)

target_link_libraries(PadCalibration PRIVATE
    facelogin_camera
    facelogin_inference
    ole32
)

set_target_properties(PadCalibration PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
