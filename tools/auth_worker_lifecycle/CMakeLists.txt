# Development-only supervisor/Job/protocol lifecycle test with an embedded
# mock -auth-worker mode. No model or camera files are required.
add_executable(AuthWorkerLifecycleTest
    auth_worker_lifecycle_test.cpp
    ${CMAKE_SOURCE_DIR}/face_service/auth_worker_client.cpp
    ${CMAKE_SOURCE_DIR}/face_service/auth_worker_protocol.cpp
)

target_link_libraries(AuthWorkerLifecycleTest PRIVATE
    facelogin_common
    onnxruntime::onnxruntime
    psapi
)

target_include_directories(AuthWorkerLifecycleTest PRIVATE
    ${CMAKE_SOURCE_DIR}/face_service
)

set_target_properties(AuthWorkerLifecycleTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
