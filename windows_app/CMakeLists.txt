cmake_minimum_required(VERSION 3.16)

project(audio_win_app CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# 显式加载器 (接入层, 与 sdk/ 平级)。INTERFACE 库: 只带 include 路径, 不产出二进制,
# 所以它不会把 audio_sdk.dll / .lib 拉进来 —— 和本程序"不链导入库"的做法是一致的。
# 加载器需要哪些目录由它自己声明(含 ../sdk/include)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk_loader
    ${CMAKE_BINARY_DIR}/sdk_loader
)

# UI 可执行程序: main.cpp(入口) + main_window.cpp(窗口类实现) + crash_dump.cpp(崩溃转储)
add_executable(audio_win_app
    main.cpp
    main_window.cpp
    crash_dump.cpp
)

set_target_properties(audio_win_app PROPERTIES WIN32_EXECUTABLE ON)

# 本工程自己的头。SDK 契约与加载器的路径由 audio_sdk_loader 那个 INTERFACE 目标带进来。
target_include_directories(audio_win_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# audio_sdk_loader 是 INTERFACE 库(只有头), 链接行上不会多出任何东西, 放心写在一起。
# 显式加载【不需要】导入库(audio_sdk.lib): 运行期靠 LoadLibrary + GetProcAddress 自己找符号。
target_link_libraries(audio_win_app PRIVATE
    audio_sdk_loader
    comdlg32
)

# dll: 运行时 LoadLibrary 会先在 exe 所在目录找, 构建后自动拷到 exe 旁边, 保证双击就能跑
add_custom_command(TARGET audio_win_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/resource/audio_sdk.dll
        $<TARGET_FILE_DIR:audio_win_app>
)
