find_package(Opus CONFIG REQUIRED)

add_library(lilypad_common STATIC
    network.cpp
    audio_codec.cpp
)

target_include_directories(lilypad_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lilypad_common PUBLIC Opus::opus ws2_32)
