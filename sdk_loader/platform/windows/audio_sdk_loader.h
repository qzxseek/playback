/* @Created On : 2026/9/18
   @Author : 孟源
   @note : 显式加载器的 Windows 平台原语。
          只装"开库 / 取符号 / 关库"这三个动作的 Windows 写法, 加上库句柄类型和默认库名。
          函数指针表(AudioSdkApi)是两平台共用的, 不在本文件 ——
          见 sdk_loader/audio_sdk_loader.h
*/
#pragma once

#ifndef _WIN32
  #error "audio_sdk_loader.h: 这是 Windows 平台原语, 不接受其它平台编译。请检查 __ANDROID__ 宏是否被误定义, 或 include 路径里混进了 sdk_loader/platform/windows。"
#endif

#include <windows.h>     
#include <cstddef>       
#include <cstdio>       

/// 动态库句柄类型; 为空表示还没加载 / 加载失败
using AudioSdkModuleHandle = HMODULE;

/// 库名(或完整路径)参数类型 —— Windows 的 API 收宽字符
using AudioSdkModuleName = const wchar_t*;

#define AUDIO_SDK_MODULE_NAME  L"audio_sdk.dll"    // 默认库名

// 把"开库 / 取符号 / 关库"收成三个宏, 好让共用的加载器代码不被 #if 切碎
#define AUDIO_SDK_OPEN(m, n)   (m) = ::LoadLibraryW(n)
#define AUDIO_SDK_SYM(m, s)    ::GetProcAddress(m, s)
#define AUDIO_SDK_CLOSE(m)     ::FreeLibrary(m)

/**
 * @brief 开库失败的提示语, 写进 AudioSdkApi::m_lastError
 * @param buf 目标缓冲
 * @param cap 缓冲容量
 * @param moduleName 当初想开的库名
 * @note 叫 AudioSdkFormatOpenError 而不是 FormatOpenError: 这是全局作用域的自由函数,
 *       名字太泛容易和别人撞。
 */
inline void AudioSdkFormatOpenError(char* buf, std::size_t cap,
                                    AudioSdkModuleName moduleName)
{
    std::snprintf(buf, cap, "LoadLibrary(\"%ls\") failed, GetLastError=%lu",
                  moduleName, ::GetLastError());
}
