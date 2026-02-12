find_package(portaudio CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_executable(lilypad_client WIN32
    main.cpp
    connection.cpp
    d3d_helpers.cpp
    network_threads.cpp
    persistence.cpp
    screen_threads.cpp
    theme.cpp
    update_checker.cpp
    audio.cpp
    screen_capture.cpp
    system_audio.cpp
    h264_encoder.cpp
    h264_decoder.cpp
    lilypad_client.rc
)

target_link_libraries(lilypad_client PRIVATE
    lilypad_common
    portaudio_static
    imgui::imgui
    rnnoise
    d3d11
    dxgi
    ole32
    mmdevapi
    uuid
    wininet
    mf
    mfplat
    mfuuid
    dwmapi
)
