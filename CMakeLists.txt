cmake_minimum_required(VERSION 3.20)
project(FaceLogin VERSION 1.6.0 LANGUAGES CXX)

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
    add_compile_options(/W4 /WX-)
    # /MP (compiler multiprocess mode) is unreliable on VS 18 / MSVC 19.5x —
    # it can leave cl.exe permanently idle. Keep /MP for older toolsets only.
    if(MSVC_VERSION LESS 1950)
        add_compile_options(/MP)
    else()
        # VS 18 (VS 2026): disable /MP plus the Tracker and error telemetry that
        # orphan cl.exe at zero CPU. These project-global switches are written
        # into each .vcxproj and honored by MSBuild, so no command-line
        # --parallel 1 is needed (verified 2026-08-09 with a clean rebuild).
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
add_subdirectory(tools/pad_calibration)
add_subdirectory(tools/embedding_test)
add_subdirectory(tools/camera_lifecycle)
add_subdirectory(tools/pipe_lifecycle)
add_subdirectory(tools/ort_memory)
