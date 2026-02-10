find_package(portaudio CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(libjpeg-turbo CONFIG REQUIRED)

add_executable(lilypad_client WIN32
    main.cpp
    audio.cpp
    screen_capture.cpp
    system_audio.cpp
)

target_link_libraries(lilypad_client PRIVATE
    lilypad_common
    portaudio
    imgui::imgui
    rnnoise
    d3d11
    dxgi
    ole32
    mmdevapi
    uuid
    wininet
    $<IF:$<TARGET_EXISTS:libjpeg-turbo::turbojpeg>,libjpeg-turbo::turbojpeg,libjpeg-turbo::turbojpeg-static>
)
