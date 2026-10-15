# Runs the production orchestration with deterministic inference doubles.
# No camera, model files, credentials or installed service are used.
add_executable(AuthPipelineTest
    auth_pipeline_test.cpp
    ${CMAKE_SOURCE_DIR}/face_service/auth_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/face_service/auth_frame_preparation.cpp
    ${CMAKE_SOURCE_DIR}/face_service/auth_frame_stream.cpp
)

target_link_libraries(AuthPipelineTest PRIVATE
    facelogin_common facelogin_tune_state
    onnxruntime::onnxruntime
)

target_include_directories(AuthPipelineTest PRIVATE
    ${CMAKE_SOURCE_DIR}/face_service
)

set_target_properties(AuthPipelineTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
