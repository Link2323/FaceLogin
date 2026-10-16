# Assemble a single embedded page without runtime asset dependencies.
# Concatenation order IS the script's dependency order (capture_flow.js first,
# app_main.js last: top-level wiring references earlier files' functions);
# tools/enrollment_ui/capture_flow_test.cjs mirrors this list.
set(ENROLLMENT_JS_SOURCES
    js/capture_flow.js
    js/ui_common.js
    js/preview.js
    js/capture_ui.js
    js/navigation.js
    js/settings.js
    js/faces.js
    js/account_refresh.js
    js/enroll_save.js
    js/logs.js
    js/app_main.js)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/styles.css")
foreach(js IN LISTS ENROLLMENT_JS_SOURCES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${js}")
endforeach()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/styles.css" ENROLLMENT_CSS)
set(ENROLLMENT_JS "")
foreach(js IN LISTS ENROLLMENT_JS_SOURCES)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${js}" ENROLLMENT_JS_CHUNK)
    string(APPEND ENROLLMENT_JS "${ENROLLMENT_JS_CHUNK}\n")
endforeach()
configure_file(index.html.in ${CMAKE_CURRENT_BINARY_DIR}/index.html @ONLY)
# RC inputs must change with the page even when MSBuild file tracking is disabled.
file(SHA256 "${CMAKE_CURRENT_BINARY_DIR}/index.html" ENROLLMENT_PAGE_SHA256)
configure_file(resource.rc.in ${CMAKE_CURRENT_BINARY_DIR}/resource.rc @ONLY)

add_library(facelogin_enrollment_support STATIC
    session_identity.cpp
    service_client.cpp
    log_viewer.cpp
)
target_link_libraries(facelogin_enrollment_support PUBLIC facelogin_common)
target_include_directories(facelogin_enrollment_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(FaceLoginConsole
    main.cpp
    WebviewHost.cpp
    EnrollmentWizard.cpp
    preview_exposure.cpp
    preview_frame_encoder.cpp
    enrollment_capture.cpp
    enrollment_templates.cpp
    enrollment_accounts.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/resource.rc
)
facelogin_add_version_rc(FaceLoginConsole VFT_APP
    "FaceLogin enrollment console" FaceLoginConsole.exe)
configure_file(version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h @ONLY)
target_include_directories(FaceLoginConsole PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(FaceLoginConsole PRIVATE
    facelogin_enrollment_support
    facelogin_camera
    facelogin_inference
    facelogin_credential_store
    comctl32
    advapi32
    ole32
    gdi32
    crypt32
    windowscodecs
    netapi32
)

target_include_directories(FaceLoginConsole PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/webview2/include
)

# WebView2LoaderStatic.lib is gitignored (*.lib) and not committed, so a
# fresh clone would fail at link time with LNK1181. Fail at configure time
# with an actionable restore command instead.
set(WEBVIEW2_LOADER_STATIC_LIB
    ${CMAKE_CURRENT_SOURCE_DIR}/webview2/lib/WebView2LoaderStatic.lib)
if(NOT EXISTS "${WEBVIEW2_LOADER_STATIC_LIB}")
    message(FATAL_ERROR
        "WebView2LoaderStatic.lib is missing (restore once from NuGet):
"
        "  powershell -ExecutionPolicy Bypass -File "
        "${CMAKE_SOURCE_DIR}/scripts/restore_webview2.ps1")
endif()

# Link WebView2 loader static library
target_link_libraries(FaceLoginConsole PRIVATE
    ${WEBVIEW2_LOADER_STATIC_LIB}
)

# WebView2Loader is statically linked (WebView2LoaderStatic.lib) — no DLL needed

set_target_properties(FaceLoginConsole PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# Embed DPI-aware manifest for sharp text on high-DPI displays
target_sources(FaceLoginConsole PRIVATE FaceLoginEnrollment.manifest)
set_source_files_properties(FaceLoginEnrollment.manifest PROPERTIES
    VS_TOOL_OVERRIDE "manifest"
    HEADER_FILE_ONLY TRUE
)

# Custom output directory
set_target_properties(FaceLoginConsole PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/installer/FaceLoginSetup/resources"
)

# assets/models is the sole source for every runtime ONNX model. Generate the
# installer's ignored packaging copies whenever the enrollment console builds;
# exact go:embed patterns make an incomplete model set a hard build failure.
add_custom_command(TARGET FaceLoginConsole POST_BUILD
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_SOURCE_DIR}/scripts/stage_licenses.ps1"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_SOURCE_DIR}/installer/FaceLoginSetup/resources/models"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/assets/models/det_10g_gnkps.onnx"
            "${CMAKE_SOURCE_DIR}/installer/FaceLoginSetup/resources/models/det_10g_gnkps.onnx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/assets/models/face_recognition_sface_2021dec.onnx"
            "${CMAKE_SOURCE_DIR}/installer/FaceLoginSetup/resources/models/face_recognition_sface_2021dec.onnx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/assets/models/MiniFASNetV2.onnx"
            "${CMAKE_SOURCE_DIR}/installer/FaceLoginSetup/resources/models/MiniFASNetV2.onnx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/assets/models/MiniFASNetV1SE.onnx"
            "${CMAKE_SOURCE_DIR}/installer/FaceLoginSetup/resources/models/MiniFASNetV1SE.onnx"
    COMMENT "Staging license documents and canonical runtime models"
)
