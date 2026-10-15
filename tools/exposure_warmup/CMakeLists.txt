# Development-only exposure warmup state machine test (+ optional --camera
# live DirectShow probe). This target is never shipped.
add_executable(ExposureWarmupTest
    exposure_warmup_test.cpp
)

target_link_libraries(ExposureWarmupTest PRIVATE facelogin_camera)
