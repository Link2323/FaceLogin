# Sensor state persistence has no camera or inference dependency.
add_library(facelogin_tune_state STATIC tune_state.cpp tune_state.h)
target_include_directories(facelogin_tune_state PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Shared by service, enrollment and diagnostics. Keep capture independent of
# ONNX Runtime so camera-only tools do not acquire inference dependencies.
add_library(facelogin_camera STATIC
    webcam_capture_dshow.cpp
    webcam_capture_dshow.h
    camera_types.h
    exposure_warmup.h
)
target_include_directories(facelogin_camera PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(facelogin_camera
    PUBLIC facelogin_common
    PRIVATE strmiids ole32 oleaut32
)

add_library(facelogin_inference STATIC
    onnx_models.cpp
    onnx_models.h
    face_align.h
    face_gain_tune.h
    liveness_types.h
)
target_include_directories(facelogin_inference PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(facelogin_inference PUBLIC facelogin_common facelogin_tune_state onnxruntime::onnxruntime)
