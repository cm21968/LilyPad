cmake_minimum_required(VERSION 3.20)
project(LilyPad LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(thirdparty/rnnoise)
add_subdirectory(common)
add_subdirectory(server)
add_subdirectory(client)
