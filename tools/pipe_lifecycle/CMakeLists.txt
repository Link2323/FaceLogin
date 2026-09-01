# Development-only pipe ACL lifecycle probe. This target is never shipped.
add_executable(PipeLifecycleTest
    pipe_lifecycle.cpp
    ${CMAKE_SOURCE_DIR}/face_service/pipe_server.cpp
    ${CMAKE_SOURCE_DIR}/credential_provider/pipe_client.cpp
)

target_link_libraries(PipeLifecycleTest PRIVATE
    facelogin_common
    advapi32
    psapi
)

target_include_directories(PipeLifecycleTest PRIVATE
    ${CMAKE_SOURCE_DIR}/face_service
    ${CMAKE_SOURCE_DIR}/credential_provider
)

set_target_properties(PipeLifecycleTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
