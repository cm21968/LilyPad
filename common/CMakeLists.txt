find_package(Opus CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(lilypad_common STATIC
    network.cpp
    audio_codec.cpp
    tls_socket.cpp
)

target_include_directories(lilypad_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lilypad_common PUBLIC Opus::opus OpenSSL::SSL OpenSSL::Crypto ws2_32 crypt32)
