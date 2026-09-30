#pragma once

/* @Created On : 2026/9/30
   @Author : 孟源
   @note : 日志 —— 记关键步骤与报错, 落成文本文件。

           文件落在【调用方 exe 所在目录】, 名字 audio_sdk_YYYYMMDD.txt, 一天一个。
           没有配置开关: 目录和级别都写死。曾经有过 LogInit/LogSetLevel, 但没有任何
           地方调用(也调不到), 那就是死代码而不是旋钮 —— 真需要改, 改本文件的默认值。

           记什么(判断标准: 崩了之后它能不能帮你缩小排查范围):
             状态转换  开录/停录/起播/暂停/停止/跳转  —— 事后无法重建"当时点了哪个"
             失败      错误码 + 原始平台码(MMRESULT/Win32) —— 枚举把平台错误压扁了,
                       只写 DEVICE_BUSY 等于没说, 得知道底下是 MMSYSERR_ALLOCATED(4)
             产物      落盘路径 —— 只有当时知道

           不记什么(事后查得到, 记了就是刷屏):
             播放位置 / 已录时长 / 每个喂给声卡的音频块 / 波形数据

           【音频线程不许调用】waveIn/waveOut 回调和 FeedLoop 是实时线程 ——
           在那里做一次阻塞写, 就是当年那个卡顿(buffer underrun)的翻版。
           本模块用一把锁, 只在控制路径(UI 线程)上用。

           注意: 文件是【第一次写日志时】才建的。全程没有状态转换(比如只启动没播放)
           就不会有文件, 这是懒打开的正常表现。
*/
#include <cstdarg>

// 让编译器按 printf 的规则校验格式串 —— 参数个数/类型对不上会在编译期报出来,
// 而不是等到线上打出一条乱码。MSVC 不认这个属性, 空展开即可(编辑器里 clangd 仍会查)。
#if defined(__GNUC__) || defined(__clang__)
#  define AUDIO_LOG_PRINTF_ATTR(fmtIdx, argIdx) __attribute__((format(printf, fmtIdx, argIdx)))
#else
#  define AUDIO_LOG_PRINTF_ATTR(fmtIdx, argIdx)
#endif

namespace AudioSdk {

enum class LogLevel { Debug = 0, Info, Warn, Error };

// 底层写入。一般不直接调, 用下面的宏。
void LogWrite(LogLevel level, const char* tag, const char* fmt, ...) AUDIO_LOG_PRINTF_ATTR(3, 4);

}   // namespace AudioSdk

#define AUDIO_LOG_DEBUG(tag, ...) ::AudioSdk::LogWrite(::AudioSdk::LogLevel::Debug, (tag), __VA_ARGS__)
#define AUDIO_LOG_INFO(tag, ...)  ::AudioSdk::LogWrite(::AudioSdk::LogLevel::Info,  (tag), __VA_ARGS__)
#define AUDIO_LOG_WARN(tag, ...)  ::AudioSdk::LogWrite(::AudioSdk::LogLevel::Warn,  (tag), __VA_ARGS__)
#define AUDIO_LOG_ERROR(tag, ...) ::AudioSdk::LogWrite(::AudioSdk::LogLevel::Error, (tag), __VA_ARGS__)
