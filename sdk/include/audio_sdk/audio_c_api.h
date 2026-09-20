/* @Created On : 2026/9/12
   @Author : 孟源
   @note : audio_sdk 的接口契约 —— 所有平台共用这一份, 由 SDK 提供。
          只回答"SDK 能做什么", 不回答"怎么找到 SDK"。

          里面两块:
            1) 导出函数声明(extern "C" 名字不修饰 + AUDIO_API 导出标记)
            2) 音频参数与状态码 —— 直接用 SDK 的 audio_types.h, 不抄第二份

          本目录(sdk/include)【只放平台无关的头】, 一个 windows.h / dlfcn.h 都不碰:
          定义侧 src/common/audio_c_api.cpp 也 include 它, 不该被平台负担拖累。
          【怎么找到 SDK】不在这里 —— 见 sdk_loader/audio_sdk_loader.h 的 AudioSdkApi。

          Android 前提(编译期检查见下): API 26(8.0)及以上 —— AAudio 从 API 26 才有。
*/
#pragma once

#include "audio_sdk/audio_export.h"
#include "audio_sdk/audio_types.h"

#include <stdint.h>

// ==================== 平台版本要求 ====================
#if defined(__ANDROID__)
  #if !defined(__ANDROID_API__)
    #error "audio_sdk: __ANDROID_API__ is not defined. Use the NDK toolchain (it derives this from -target ...androidNN), or pass -D__ANDROID_API__=26."
  #elif __ANDROID_API__ < 26
    #error "audio_sdk: Android API 26 (8.0) or newer is required - AAudio was introduced in API 26 and libaaudio.so does not exist below it. Set minSdkVersion 26 in build.gradle, or pass -DANDROID_PLATFORM=android-26 to CMake."
  #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 导出函数声明 ====================

// ==================== 播放器 ====================

// 创建播放器对象; 成功返回句柄(非空), 失败返回 NULL
AUDIO_API void* AudioSdk_PlayerCreate(void);

// 销毁播放器对象(句柄可传 NULL, 安全)
AUDIO_API void AudioSdk_PlayerDestroy(void* handle);

// 打开并播放 .wav/.aenc(UTF-8 路径); 返回 AudioSdkState 的序号
AUDIO_API int AudioSdk_PlayerPlayFile(void* handle, const char* utf8Path);

AUDIO_API void AudioSdk_PlayerPausePlay(void* handle);
AUDIO_API void AudioSdk_PlayerResumePlay(void* handle);
AUDIO_API void AudioSdk_PlayerStopPlay(void* handle);

// 跳转播放位置(字节); 返回 AudioSdkState 的序号
AUDIO_API int AudioSdk_PlayerSeek(void* handle, uint32_t posBytes);

AUDIO_API uint32_t AudioSdk_PlayerGetPlayPos(void* handle);    // 已播字节
AUDIO_API uint32_t AudioSdk_PlayerGetTotalPos(void* handle);   // 总字节
AUDIO_API int      AudioSdk_PlayerIsPlaying(void* handle);     // 1=在播, 0=否
AUDIO_API uint32_t AudioSdk_PlayerGetPlayPosMs(void* handle);   // 当前播放位置(毫秒)
AUDIO_API uint32_t AudioSdk_PlayerGetTotalPosMs(void* handle);  // 总时长(毫秒); 未加载文件为 0

// 取整个文件的波形(降采样成 CWaveform::kFilePoints 个峰值点)。
// 在【调用线程】同步回调一次(通常就是 UI 线程), 无并发问题。
// 回调返回后缓冲即失效, 需要就自己拷走。
AUDIO_API void AudioSdk_PlayerBuildWaveform(void* handle,
                                            AudioSdkWaveCallback cb,
                                            void* userData);

// ==================== 录音器 ====================

// 创建录音器对象; 成功返回句柄(非空), 失败返回 NULL
AUDIO_API void* AudioSdk_RecorderCreate(void);

// 销毁录音器对象(句柄可传 NULL, 安全)
AUDIO_API void AudioSdk_RecorderDestroy(void* handle);

// 开始录音; 返回 AudioSdkState 的序号
AUDIO_API int AudioSdk_RecorderStart(void* handle);

// 暂停/继续录音
AUDIO_API void AudioSdk_RecorderPauseResume(void* handle);

// 停止录音并落盘; 返回 AudioSdkState 的序号
AUDIO_API int AudioSdk_RecorderStop(void* handle);

AUDIO_API void AudioSdk_RecorderSetOutputPath(void* handle, const char* utf8Path);

// 切换加密开关(录制中不生效)
AUDIO_API void AudioSdk_RecorderSetAencEncrypt(void* handle);

AUDIO_API int      AudioSdk_RecorderGetAencEncrypt(void* handle);    // 1=加密保存
AUDIO_API int      AudioSdk_RecorderGetIsPaused(void* handle);       // 1=已暂停
AUDIO_API uint32_t AudioSdk_RecorderGetRecordedMs(void* handle);     // 已录时长(毫秒)

// 注册/取消录音波形回调(cb 传 NULL 取消), 录制中每积累一块就回调一次。
// 回调跑在【音频线程】: 只许做"拷贝数据 + PostMessage", 禁止分配内存/加锁/操作 UI。
// 停止录音前建议先传 NULL 取消注册。
AUDIO_API void AudioSdk_RecorderSetWaveCallback(void* handle,
                                                AudioSdkWaveCallback cb,
                                                void* userData);

AUDIO_API int AudioSdk_IsAencFile(const char* utf8Path);   // 1=加密, 0=否

#ifdef __cplusplus
}
#endif
