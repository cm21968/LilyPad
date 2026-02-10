cmake_minimum_required(VERSION 3.20)

add_library(rnnoise STATIC
    src/denoise.c
    src/celt_lpc.c
    src/kiss_fft.c
    src/nnet.c
    src/nnet_default.c
    src/pitch.c
    src/rnn.c
    src/rnnoise_tables.c
    src/parse_lpcnet_weights.c
)

target_include_directories(rnnoise PUBLIC include)
target_include_directories(rnnoise PRIVATE src)

target_compile_definitions(rnnoise PRIVATE
    COMPILE_OPUS
    HAVE_STDINT_H
)

if(MSVC)
    target_compile_options(rnnoise PRIVATE /wd4244 /wd4305)
endif()
