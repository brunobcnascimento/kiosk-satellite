# Speex DSP (xiph.org, BSD): the multichannel MDF echo filter behind the
# kiosk's echo canceller (echo_canceller.cpp), its noise suppressor and the
# real FFT both share with the canceller's mask. Built from the pinned
# sources with the same options the project's autotools build would pick
# for a floating point target: no config.h, so the two feature macros come
# from here, and the types header the build would generate is written
# from its template.

include(FetchContent)

# SpeexDSP-1.2.1
FetchContent_Declare(
    speexdsp
    GIT_REPOSITORY https://github.com/xiph/speexdsp.git
    GIT_TAG        1b28a0f61bc31162979e1f26f3981fc3637095c8
)
FetchContent_GetProperties(speexdsp)
if(NOT speexdsp_POPULATED)
    FetchContent_Populate(speexdsp)
endif()

set(INCLUDE_STDINT "#include <stdint.h>")
set(SIZE16 int16_t)
set(USIZE16 uint16_t)
set(SIZE32 int32_t)
set(USIZE32 uint32_t)
configure_file(
    ${speexdsp_SOURCE_DIR}/include/speex/speexdsp_config_types.h.in
    ${speexdsp_BINARY_DIR}/include/speex/speexdsp_config_types.h
    @ONLY)

add_library(speexdsp STATIC
    ${speexdsp_SOURCE_DIR}/libspeexdsp/mdf.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/preprocess.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/filterbank.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/fftwrap.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/kiss_fft.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/kiss_fftr.c)
target_compile_definitions(speexdsp PUBLIC FLOATING_POINT USE_KISS_FFT EXPORT=)
target_compile_definitions(speexdsp PRIVATE VAR_ARRAYS)
# The MDF filter's spectral loops vectorize under fast math: a third less
# time on a Cortex-A53, with the filter's output within rounding of the
# strict build (69 dB down on a captured conversation).
target_compile_options(speexdsp PRIVATE -w -O3 -ffast-math)
target_include_directories(speexdsp PUBLIC
    ${speexdsp_SOURCE_DIR}/include
    ${speexdsp_BINARY_DIR}/include
    ${speexdsp_BINARY_DIR}/include/speex
    ${speexdsp_SOURCE_DIR}/libspeexdsp)
