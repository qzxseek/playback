cmake_minimum_required(VERSION 3.16)

project(audio_win_app CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# UI 可执行程序: main.cpp(入口) + main_window.cpp(窗口类实现)
add_executable(audio_win_app
    main.cpp
    main_window.cpp
)

set_target_properties(audio_win_app PROPERTIES WIN32_EXECUTABLE ON)

# ① 头文件: 直接用 SDK 的头(sdk/include), 不再维护 UI 侧副本, 避免两份漂移
target_include_directories(audio_win_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include
)

# ② 导入库 .lib: 链接期告诉链接器"这些符号在 audio_sdk.dll 里"
target_link_libraries(audio_win_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/resource/audio_sdk.lib
    comdlg32
)

# ③ dll: exe 启动时按"exe 同目录 → 系统目录 → PATH"查找,
#    这里构建后自动把 resource/audio_sdk.dll 拷到 exe 旁边, 保证独立运行
add_custom_command(TARGET audio_win_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/resource/audio_sdk.dll
        $<TARGET_FILE_DIR:audio_win_app>
)
