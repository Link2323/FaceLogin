# Development-only public Credential Provider IPC compatibility test.
add_executable(IpcProtocolTest
    ipc_protocol_test.cpp
)

target_link_libraries(IpcProtocolTest PRIVATE facelogin_common)

set_target_properties(IpcProtocolTest PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
