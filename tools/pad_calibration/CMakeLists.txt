add_executable(PadCalibration
    main.cpp
)

target_link_libraries(PadCalibration PRIVATE
    facelogin_camera
    facelogin_inference
    ole32
)
