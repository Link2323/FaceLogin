add_executable(PadCalibration
    main.cpp
    ${CMAKE_SOURCE_DIR}/face_service/webcam_capture.cpp
    ${CMAKE_SOURCE_DIR}/face_service/webcam_capture_dshow.cpp
    ${CMAKE_SOURCE_DIR}/face_service/onnx_models.cpp
)

target_link_libraries(PadCalibration PRIVATE
    facelogin_common

    onnxruntime::onnxruntime
    mfplat
    mf
    mfreadwrite
    mfuuid
    strmiids
    ole32
)

target_include_directories(PadCalibration PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/face_service
)

set_target_properties(PadCalibration PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
