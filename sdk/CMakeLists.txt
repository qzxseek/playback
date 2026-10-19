cmake_minimum_required(VERSION 3.16)

project(audio_sdk CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /wd4819)
endif()

# 平台相关 
if(WIN32)
    set(PLATFORM_SOURCES
        src/windows/audio_player.cpp
        src/windows/audio_recorder.cpp)
    set(PLATFORM_LIBS winmm)
elseif(ANDROID)
    set(PLATFORM_SOURCES
        src/android/audio_player.cpp
        src/android/audio_recorder.cpp)
    find_library(aaudio-lib aaudio)          
    set(PLATFORM_LIBS ${aaudio-lib})
else()
    message(FATAL_ERROR "audio_sdk: 暂不支持的平台 ${CMAKE_SYSTEM_NAME}")
endif()

# 平台无关的公共源。抽成变量是必要的: 之前每个目标各抄一份列表,
# 新加 waveform.cpp 时漏改了两个测试目标 → LNK2019。集中一处就不会再漏。
set(SDK_COMMON_SOURCES
    src/common/wav_format.cpp
    src/common/encrypted_format.cpp
    src/common/ogg_format.cpp      # ogg/vorbis 编解码(压缩格式层)
    src/common/waveform.cpp        # 波形峰值计算(两平台共用)
    src/common/wave_ring.cpp       # 波形环形缓冲(两平台共用, 实现自 wave_ring.h 抽出)
    src/common/byte_ring.cpp       # 字节环形缓冲(流式播放: 生产者读文件 / 音频回调取数据)
    logging/src/logging.cpp        # 日志(两平台共用)
)

# SDK 动态库
add_library(audio_sdk SHARED
    ${PLATFORM_SOURCES}
    ${SDK_COMMON_SOURCES}
    src/common/wav_validate.cpp
    src/common/audio_c_api.cpp     # C 接口层(extern "C" 导出, 供 Windows UI 显式加载)
    src/common/opus_format.cpp     # opus 编码(依赖下面的 opus 包 + opusenc 目标)
)
# 注: opus_format.cpp 没进 SDK_COMMON_SOURCES —— 测试目标不碰 opus,
# 拖进去就得让四个测试也链 opus 包。将来测试要用 opus 时再挪进公共列表。

target_include_directories(audio_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/logging/include
)
target_link_libraries(audio_sdk PUBLIC ${PLATFORM_LIBS})

# 告诉 audio_export.h: 现在编的是 SDK 本体 → AUDIO_API 展开成 dllexport
target_compile_definitions(audio_sdk PRIVATE AUDIO_SDK_BUILD)

if(MSVC)
    target_compile_options(audio_sdk PRIVATE /Zi)
    target_link_options(audio_sdk PRIVATE /DEBUG)
endif()

if(WIN32)
    add_custom_command(TARGET audio_sdk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:audio_sdk>"
                "${CMAKE_CURRENT_SOURCE_DIR}/../windows_app/resource/audio_sdk.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_PDB_FILE:audio_sdk>"
                "${CMAKE_CURRENT_SOURCE_DIR}/../windows_app/resource/audio_sdk.pdb"
        # opus 只有动态库(vcpkg 那份是 SHARED IMPORTED), 少了它 audio_sdk.dll 直接加载不起来
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Opus::opus>"
                "${CMAKE_CURRENT_SOURCE_DIR}/../windows_app/resource/opus.dll"
        COMMENT "发布 audio_sdk.dll / audio_sdk.pdb / opus.dll 到 windows_app/resource"
    )
endif()

# 音频编码解码库(Vorbis)
if(WIN32)
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/vorbis")
elseif(ANDROID)
    set(Vorbis_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/vorbis/lib/cmake/Vorbis")
endif()

find_package(Vorbis CONFIG REQUIRED)
target_link_libraries(audio_sdk PRIVATE Vorbis::vorbisenc Vorbis::vorbisfile)

# 音频编码解码库(Opus)
if(WIN32)
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/opus")
elseif(ANDROID)
    set(Opus_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/opus/lib/cmake/opus")
endif()

find_package(Opus CONFIG REQUIRED)
target_link_libraries(audio_sdk PRIVATE Opus::opus)

# libopusenc: "opus 的 ogg 封装"(源码在 tool/libopusenc)
add_library(opusenc STATIC
    tool/libopusenc/src/ogg_packer.c
    tool/libopusenc/src/opus_header.c
    tool/libopusenc/src/opusenc.c
    tool/libopusenc/src/picture.c
    tool/libopusenc/src/resample.c
    tool/libopusenc/src/unicode_support.c
)

target_include_directories(opusenc
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/tool/libopusenc/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tool/libopusenc/include/opus
            ${CMAKE_CURRENT_SOURCE_DIR}/tool/libopusenc/src       # 内部头 + resample
)

target_compile_definitions(opusenc PRIVATE
    OUTSIDE_SPEEX
    FLOATING_POINT
    RANDOM_PREFIX=libopusenc
    PACKAGE_NAME="libopusenc"
    PACKAGE_VERSION="0.3"
)
target_link_libraries(opusenc PRIVATE Opus::opus)

target_link_libraries(audio_sdk PRIVATE opusenc)

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
if(WIN32)
    set(SDK_TEST_SOURCES
        test/src/test_positive.cpp
        test/src/test_negative.cpp
        test/src/test_boundary.cpp
        test/src/test_abnormal.cpp
    )

    foreach(test_src IN LISTS SDK_TEST_SOURCES)
        get_filename_component(test_name ${test_src} NAME_WE)
        add_executable(${test_name}
            ${test_src}
            ${PLATFORM_SOURCES}
            ${SDK_COMMON_SOURCES}
            src/common/wav_validate.cpp
        )
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/logging/include
            ${CMAKE_CURRENT_SOURCE_DIR}/test
        )

        target_link_libraries(${test_name} PRIVATE
            ${PLATFORM_LIBS}
            Vorbis::vorbisenc Vorbis::vorbisfile)
        target_compile_definitions(${test_name} PRIVATE AUDIO_SDK_STATIC)
        set_target_properties(${test_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/test
        )
    endforeach()
endif()
