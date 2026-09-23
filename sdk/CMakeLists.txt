cmake_minimum_required(VERSION 3.16)

project(audio_sdk CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
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
    src/common/waveform.cpp        # 波形峰值计算(两平台共用)
    src/common/wave_ring.cpp       # 波形环形缓冲(两平台共用, 实现自 wave_ring.h 抽出)
)

# SDK 动态库
add_library(audio_sdk SHARED
    ${PLATFORM_SOURCES}
    ${SDK_COMMON_SOURCES}
    src/common/wav_validate.cpp
    src/common/audio_c_api.cpp     # C 接口层(extern "C" 导出, 供 Windows UI 显式加载)
)

target_include_directories(audio_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(audio_sdk PUBLIC ${PLATFORM_LIBS})

# 告诉 audio_export.h: 现在编的是 SDK 本体 → AUDIO_API 展开成 dllexport
target_compile_definitions(audio_sdk PRIVATE AUDIO_SDK_BUILD)

# Windows 专用自测程序(依赖 winmm 设备层, Android 下不构建) 
if(WIN32)
    # 录制测试：正常多 TU 链接（CAudioRecorder 类，回调走 CALLBACK_FUNCTION，无需窗口）。
    add_executable(test_win_record
        test/win_test_record.cpp
        src/windows/audio_recorder.cpp
        ${SDK_COMMON_SOURCES}
    )
    target_include_directories(test_win_record PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    # 测试程序把 SDK 源码静态编进来, 既不是导入也不是导出 → AUDIO_API 置空
    target_compile_definitions(test_win_record PRIVATE AUDIO_SDK_STATIC)
    target_link_libraries(test_win_record PRIVATE winmm)

    # 播放测试：正常多 TU 链接（CAudioPlayer 类；wav_format 提供测试音生成用的 SaveWavFile）。
    add_executable(test_win_play
        test/win_test_play.cpp
        src/windows/audio_player.cpp
        src/common/wav_validate.cpp
        ${SDK_COMMON_SOURCES}
    )
    target_include_directories(test_win_play PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    # 同上: 静态编译 SDK 源码, 不要 dllimport/dllexport
    target_compile_definitions(test_win_play PRIVATE AUDIO_SDK_STATIC)
    target_link_libraries(test_win_play PRIVATE winmm)

    # 格式层单测：不碰设备不碰线程, 只链格式层三个 .cpp, 秒级可反复跑
    add_executable(test_format_unit
        test/format_unit.cpp
        src/common/wav_validate.cpp
        ${SDK_COMMON_SOURCES}
    )
    target_include_directories(test_format_unit PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(test_format_unit PRIVATE AUDIO_SDK_STATIC)
endif()

# 发布: 把 dll 拷到 windows_app/resource, 供 UI 工程(独立 CMake 工程, 显式加载)取用
if(WIN32)
    add_custom_command(TARGET audio_sdk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:audio_sdk>"
                "${CMAKE_CURRENT_SOURCE_DIR}/../windows_app/resource/audio_sdk.dll"
        COMMENT "发布 audio_sdk.dll 到 windows_app/resource"
    )
endif()
