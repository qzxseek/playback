/* @Created On : 2026/9/18
   @Author : 孟源
   @note : 显式加载器的 Android 平台原语。
          只装"开库 / 取符号 / 关库"这三个动作的 Android 写法, 加上库句柄类型和默认库名。
          函数指针表(AudioSdkApi)是两平台共用的, 不在本文件 ——
          见 sdk_loader/audio_sdk_loader.h

          本文件不直接被调用方 include: 由上一层 audio_sdk_loader.h 按平台选路径带进来。
          怎么用见 sdk_loader/audio_sdk_loader.h 顶部的说明
*/
#pragma once

// 反向守卫: 这份是 Android 原语, 被别的平台编到就当场报错
// 正常情况下轮不到它 —— audio_sdk_loader.h 按 _WIN32/__ANDROID__ 选路径
// 这里是防"include 路径配错、把另一平台的头拖进来"
#ifndef __ANDROID__
  #error "audio_sdk_loader.h: 这是 Android 平台原语, 不接受其它平台编译。请检查 include 路径里是不是混进了 sdk_loader/platform/android。"
#endif

#include <dlfcn.h>       // dlopen / dlsym / dlclose / dlerror / RTLD_NOW / RTLD_LOCAL
#include <cstddef>       
#include <cstdio>        

/// 动态库句柄类型; 为空表示还没加载 / 加载失败
using AudioSdkModuleHandle = void*;

/// 库名(或完整路径)参数类型
using AudioSdkModuleName = const char*;

#define AUDIO_SDK_MODULE_NAME  "libaudio_sdk.so"    // 默认库名

// 把"开库 / 取符号 / 关库"收成三个宏, 好让共用的加载器代码不被 #if 切碎
// RTLD_LOCAL: 符号不导给别人, 免得和桥自己的符号撞名
#define AUDIO_SDK_OPEN(m, n)   (m) = ::dlopen(n, RTLD_NOW | RTLD_LOCAL)
#define AUDIO_SDK_SYM(m, s)    ::dlsym(m, s)
#define AUDIO_SDK_CLOSE(m)     ::dlclose(m)

/**
 * @brief 开库失败的提示语, 写进 AudioSdkApi::m_lastError
 * @param buf 目标缓冲
 * @param cap 缓冲容量
 * @param moduleName 当初想开的库名
 * @note dlopen 失败时 dlerror() 里有一条人话原因, 比 errno 好使得多, 顺手带上。
 *       注意 dlerror() 读一次就清空, 所以只调一次、存进变量再用。
 */
inline void AudioSdkFormatOpenError(char* buf, std::size_t cap,
                                    AudioSdkModuleName moduleName)
{
    const char* reason = ::dlerror();
    std::snprintf(buf, cap, "dlopen(\"%s\") failed: %s",
                  moduleName, reason ? reason : "unknown");
}
