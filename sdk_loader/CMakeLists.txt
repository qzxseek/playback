cmake_minimum_required(VERSION 3.16)

# 显式加载器: "怎么找到 SDK"。属于调用方的接入层, 不是 SDK 本体 ——
# 所以摆在 sdk/ 旁边, 而不是混在 sdk/ 里面。
#
# 这里【不写 project()】: 本文件只会被调用方 add_subdirectory 进来, 自己没有可构建的目标。
# 写了 project() 会让 CMake 在这个子目录里再生成一套 ALL_BUILD.vcxproj / .sln, 白添迷惑。

# INTERFACE 库 = 只有头文件、没有源码、不产出任何二进制。
# 调用方用 target_link_libraries(<你的目标> PRIVATE audio_sdk_loader) 把它接进去。
add_library(audio_sdk_loader INTERFACE)

# 两个 include 路径, 缺一不可:
#   sdk_loader        —— 加载器自己 audio_sdk_loader.h(调用方 include 的就是它)
#   ../sdk/include    —— 它要 include 的契约, 指针表用 decltype(&::AudioSdk_Xxx) 读签名
# 不需要再加 sdk_loader/platform: platform/windows/... 那句是引号 include,
# 会先搜包含它的文件所在目录, 也就是 sdk_loader/platform/ 里, 自然找得到。
target_include_directories(audio_sdk_loader INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/include
)
