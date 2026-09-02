cmake_minimum_required(VERSION 3.20)
project(FaceLogin VERSION 1.8.0 LANGUAGES CXX)

# Stamps a VERSIONINFO resource (project() version) into a Windows target.
# Usage after add_executable/add_library:
#   facelogin_add_version_rc(<target> VFT_APP|VFT_DLL "<description>" <original.exe>)
function(facelogin_add_version_rc target filetype description originalname)
    set(RC_FILEVERSION ${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0)
    set(RC_FILEVERSION_STR ${PROJECT_VERSION})
    set(RC_FILETYPE ${filetype})
    set(RC_FILEDESCRIPTION ${description})
    set(RC_INTERNALNAME ${target})
    set(RC_ORIGINALFILENAME ${originalname})
    configure_file(${PROJECT_SOURCE_DIR}/version.rc.in
                   ${CMAKE_CURRENT_BINARY_DIR}/version_${target}.rc @ONLY)
    target_sources(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version_${target}.rc)
endfunction()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(onnxruntime CONFIG REQUIRED)

# Global compile definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

# Global compiler options
if(MSVC)
    add_compile_options(/W4 /WX- /MP)
    # VS 18 (VS 2026): /MP used to orphan cl.exe at zero CPU while the MSBuild
    # Tracker was enabled. With TrackFileAccess=false + ErrorReporting=None
    # (below) /MP is stable (re-verified 2026-08-13 with repeated clean
    # rebuilds). These project-global switches are written into each .vcxproj
    # and honored by MSBuild.
    if(NOT MSVC_VERSION LESS 1950)
        list(APPEND CMAKE_VS_GLOBALS
            "ErrorReporting=None"
            "TrackFileAccess=false"
            "UseMultiToolTask=false"
        )
    endif()
    # Force MSVC to parse all source files as UTF-8 (the sources contain raw
    # non-ASCII characters like — and →). Without this, MSVC decodes them
    # using the system code page (e.g. GBK/936), producing garbled wide-char
    # literals that show up as 鈥?/鈫? in the UTF-16 log files.
    add_compile_options(/utf-8)
    # Security hardening
    add_link_options(/DYNAMICBASE /NXCOMPAT /GUARD:CF)
endif()

add_subdirectory(common)
add_subdirectory(face_service)
add_subdirectory(credential_provider)
add_subdirectory(enrollment_app)
# Development diagnostics and tests stay addressable as explicit CMake targets,
# but are excluded from the normal product build. Target-level exclusion keeps
# them available to `cmake --build ... --target <tool>` on Visual Studio.
function(add_development_tool directory target)
    add_subdirectory(${directory})
    set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
endfunction()

add_development_tool(tools/pad_calibration PadCalibration)
add_development_tool(tools/embedding_test EmbeddingTest)
add_development_tool(tools/camera_lifecycle CameraLifecycleTest)
add_development_tool(tools/pipe_lifecycle PipeLifecycleTest)
add_development_tool(tools/ort_memory OrtMemoryTest)
add_development_tool(tools/auth_worker_protocol AuthWorkerProtocolTest)
add_development_tool(tools/credential_store CredentialStoreTest)
add_development_tool(tools/auth_worker_lifecycle AuthWorkerLifecycleTest)
add_development_tool(tools/credential_provider_policy CredentialProviderInteractionPolicyTest)
add_development_tool(tools/model_integrity ModelIntegrityTest)
add_development_tool(tools/ipc_protocol IpcProtocolTest)
add_development_tool(tools/liveness_timing LivenessTimingTest)
add_development_tool(tools/exposure_warmup ExposureWarmupTest)
