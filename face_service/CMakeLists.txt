add_executable(FaceLoginService
    main.cpp
    FaceService.cpp
    auth_pipeline.cpp
    auth_worker.cpp
    auth_worker_client.cpp
    auth_worker_protocol.cpp
    onnx_models.cpp
    performance_affinity.cpp
    webcam_capture_dshow.cpp
    pipe_server.cpp
    credential_store.cpp
)

target_link_libraries(FaceLoginService PRIVATE
    facelogin_common

    onnxruntime::onnxruntime
    strmiids
    advapi32
    ole32
)

target_include_directories(FaceLoginService PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(FaceLoginService PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

# No console window for service
# set_target_properties(FaceLoginService PROPERTIES
#     WIN32_EXECUTABLE TRUE
# )
