add_library(FaceLoginCredentialProvider SHARED
    dllmain.cpp
    FaceLoginProvider.cpp
    FaceLoginCredential.cpp
    pipe_client.cpp
    resource.rc
    credential_provider.def
)

# IMPORTANT: This DLL is loaded by LogonUI.exe (lock screen) and is registered
# by the installer via an in-process DllRegisterServer call (see
# installer/FaceLoginSetup/internal/com.go). Both contexts run with a restricted
# or stripped environment block where api-ms-win-crt-* API-set resolution can
# fail (Win32 error 203), which previously broke DLL loading entirely. To
# eliminate ALL external DLL resolution beyond Windows system DLLs, this target
# uses the STATIC CRT (/MT).
#
# facelogin_common is a STATIC library compiled with /MD (to match the
# service/console + vcpkg deps), which is incompatible with this target's /MT.
# We cannot link it directly (LNK2038 RuntimeLibrary mismatch), and CMake
# OBJECT libraries do not recompile their objects per-consumer either. So we
# absorb the common sources directly via target_sources, letting them compile
# under this target's /MT setting.
target_sources(FaceLoginCredentialProvider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/ipc_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/dpapi_util.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/config_util.cpp
)

target_link_libraries(FaceLoginCredentialProvider PRIVATE
    credui
    advapi32
    ole32
    oleaut32
    kernel32
    shlwapi
    netapi32
    secur32
    crypt32
)

target_include_directories(FaceLoginCredentialProvider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

set_target_properties(FaceLoginCredentialProvider PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# UTF-8 source encoding for Chinese strings
target_compile_options(FaceLoginCredentialProvider PRIVATE /utf-8)

# Security hardening for DLL loaded into LogonUI.exe
target_link_options(FaceLoginCredentialProvider PRIVATE
    /DYNAMICBASE
    /NXCOMPAT
    /GUARD:CF
    /HIGHENTROPYVA
)

# No console
set_target_properties(FaceLoginCredentialProvider PROPERTIES
    WIN32_EXECUTABLE TRUE
)
