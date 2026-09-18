/* @Created On : 2026/9/18
   @Author : 孟源
   @note : 显式加载器 AudioSdkApi —— "怎么找到 SDK"。
          Load() 开库取符号, Unload() 关库, 平台差异全被下面的原语头吃掉了,
          所以调用方在两个平台上写的是同一行代码。

          用法(Windows / Android 一字不差):
            AudioSdkApi api;
            if (!api.Load()) { 看 api.LastError(); }
            void* player = api.PlayerCreate();
            api.PlayerPlayFile(player, "test.wav");
            api.PlayerDestroy(player);
            api.Unload();          // 调用前必须先销毁全部 handle, 否则析构会跳进已卸载的代码

          Android 侧的前提: ① API 26(8.0)及以上 —— AAudio 从 API 26 才有;
                            ② SDK 要单独编成 libaudio_sdk.so 随 APK 装进 jniLibs;
                            ③ 桥自己的 .so 不要链它。
*/
#pragma once

#include "audio_sdk/audio_c_api.h"   // 契约: 24 个 AudioSdk_* 声明(下面 decltype 要用)

#if defined(_WIN32)
  #include "windows/audio_sdk_loader.h"
#elif defined(__ANDROID__)
  #include "android/audio_sdk_loader.h"
#else
  #error "audio_sdk: 没有这个平台的显式加载器。照 sdk/platform/windows/audio_sdk_loader.h 的样子加一份 sdk/platform/<平台>/audio_sdk_loader.h, 再把平台分支加到这里, 并确认 sdk/platform 在 include 路径里。"
#endif

#ifdef __cplusplus

/**
 * @brief 从动态库里取一个函数指针
 * @param pfn 函数指针变量名
 * @param symbol 函数名(在动态库里)
 * @note 单个符号缺失不立刻返回 —— 先记下原因, 由 Load() 统一回滚。
 *       留着半套指针比直接失败更危险, 所以宁可整体作废。
 */
#define AUDIO_SDK_LOAD(pfn, symbol)                                                 \
    do {                                                                             \
        (pfn) = reinterpret_cast<decltype(pfn)>(AUDIO_SDK_SYM(hModule, symbol));      \
        if ((pfn) == nullptr) { LoadFailed(symbol); bOk = false; }                     \
    } while (0)

/**
 * @brief 从动态库里取到的全部函数指针。
 *        Load() 失败返回 false, 用 LastError() 看是哪个符号没找到。
 */
struct AudioSdkApi{
    AudioSdkModuleHandle hModule = nullptr;   // 动态库句柄; 为空表示还没加载 / 加载失败

    // —— 播放器 ——
    decltype(&::AudioSdk_PlayerCreate)            PlayerCreate        = nullptr;
    decltype(&::AudioSdk_PlayerDestroy)           PlayerDestroy       = nullptr;
    decltype(&::AudioSdk_PlayerPlayFile)          PlayerPlayFile      = nullptr;
    decltype(&::AudioSdk_PlayerPausePlay)         PlayerPausePlay     = nullptr;
    decltype(&::AudioSdk_PlayerResumePlay)        PlayerResumePlay    = nullptr;
    decltype(&::AudioSdk_PlayerStopPlay)          PlayerStopPlay      = nullptr;
    decltype(&::AudioSdk_PlayerSeek)              PlayerSeek          = nullptr;
    decltype(&::AudioSdk_PlayerGetPlayPos)        PlayerGetPlayPos    = nullptr;
    decltype(&::AudioSdk_PlayerGetTotalPos)       PlayerGetTotalPos   = nullptr;
    decltype(&::AudioSdk_PlayerIsPlaying)         PlayerIsPlaying     = nullptr;
    decltype(&::AudioSdk_PlayerGetPlayPosMs)      PlayerGetPlayPosMs  = nullptr;
    decltype(&::AudioSdk_PlayerGetTotalPosMs)     PlayerGetTotalPosMs = nullptr;
    decltype(&::AudioSdk_PlayerBuildWaveform)     PlayerBuildWaveform = nullptr;

    // —— 录音器 ——
    decltype(&::AudioSdk_RecorderCreate)          RecorderCreate          = nullptr;
    decltype(&::AudioSdk_RecorderDestroy)         RecorderDestroy         = nullptr;
    decltype(&::AudioSdk_RecorderStart)           RecorderStart           = nullptr;
    decltype(&::AudioSdk_RecorderPauseResume)     RecorderPauseResume     = nullptr;
    decltype(&::AudioSdk_RecorderStop)            RecorderStop            = nullptr;
    decltype(&::AudioSdk_RecorderSetAencEncrypt)  RecorderSetAencEncrypt  = nullptr;
    decltype(&::AudioSdk_RecorderGetAencEncrypt)  RecorderGetAencEncrypt  = nullptr;
    decltype(&::AudioSdk_RecorderGetIsPaused)     RecorderGetIsPaused     = nullptr;
    decltype(&::AudioSdk_RecorderGetRecordedMs)   RecorderGetRecordedMs   = nullptr;
    decltype(&::AudioSdk_RecorderSetWaveCallback) RecorderSetWaveCallback = nullptr;
    decltype(&::AudioSdk_IsAencFile)              IsAencFile              = nullptr;

    /**
     * @brief 显式加载: 打开动态库, 逐个取函数地址。
     *        全部取到返回 true(幂等, 重复调用不会再加载一遍)。
     * @param moduleName 库文件名或完整路径
     * @return 全部符号都取到返回 true; 任一缺失返回 false 并整体回滚
     */
    bool Load(AudioSdkModuleName moduleName = AUDIO_SDK_MODULE_NAME){
        if (hModule) return true;                     // 已经加载过
        AUDIO_SDK_OPEN(hModule, moduleName);
        if (hModule == nullptr) {
            AudioSdkFormatOpenError(m_lastError, sizeof(m_lastError), moduleName);
            return false;
        }

        bool bOk = true;

        // 播放器
        AUDIO_SDK_LOAD(PlayerCreate,        "AudioSdk_PlayerCreate");
        AUDIO_SDK_LOAD(PlayerDestroy,       "AudioSdk_PlayerDestroy");
        AUDIO_SDK_LOAD(PlayerPlayFile,      "AudioSdk_PlayerPlayFile");
        AUDIO_SDK_LOAD(PlayerPausePlay,     "AudioSdk_PlayerPausePlay");
        AUDIO_SDK_LOAD(PlayerResumePlay,    "AudioSdk_PlayerResumePlay");
        AUDIO_SDK_LOAD(PlayerStopPlay,      "AudioSdk_PlayerStopPlay");
        AUDIO_SDK_LOAD(PlayerSeek,          "AudioSdk_PlayerSeek");
        AUDIO_SDK_LOAD(PlayerGetPlayPos,    "AudioSdk_PlayerGetPlayPos");
        AUDIO_SDK_LOAD(PlayerGetTotalPos,   "AudioSdk_PlayerGetTotalPos");
        AUDIO_SDK_LOAD(PlayerIsPlaying,     "AudioSdk_PlayerIsPlaying");
        AUDIO_SDK_LOAD(PlayerGetPlayPosMs,  "AudioSdk_PlayerGetPlayPosMs");
        AUDIO_SDK_LOAD(PlayerGetTotalPosMs, "AudioSdk_PlayerGetTotalPosMs");
        AUDIO_SDK_LOAD(PlayerBuildWaveform, "AudioSdk_PlayerBuildWaveform");

        // 录音器
        AUDIO_SDK_LOAD(RecorderCreate,          "AudioSdk_RecorderCreate");
        AUDIO_SDK_LOAD(RecorderDestroy,         "AudioSdk_RecorderDestroy");
        AUDIO_SDK_LOAD(RecorderStart,           "AudioSdk_RecorderStart");
        AUDIO_SDK_LOAD(RecorderPauseResume,     "AudioSdk_RecorderPauseResume");
        AUDIO_SDK_LOAD(RecorderStop,            "AudioSdk_RecorderStop");
        AUDIO_SDK_LOAD(RecorderSetAencEncrypt,  "AudioSdk_RecorderSetAencEncrypt");
        AUDIO_SDK_LOAD(RecorderGetAencEncrypt,  "AudioSdk_RecorderGetAencEncrypt");
        AUDIO_SDK_LOAD(RecorderGetIsPaused,     "AudioSdk_RecorderGetIsPaused");
        AUDIO_SDK_LOAD(RecorderGetRecordedMs,   "AudioSdk_RecorderGetRecordedMs");
        AUDIO_SDK_LOAD(RecorderSetWaveCallback, "AudioSdk_RecorderSetWaveCallback");
        AUDIO_SDK_LOAD(IsAencFile,              "AudioSdk_IsAencFile");

        if (!bOk) { Unload(); return false; }          // 缺符号就整体回滚, 别留半套指针
        return true;
    }

    /// 卸载动态库(调用前先销毁由它创建的对象, 否则析构会跳进已卸载的代码)
    void Unload()
    {
        if (hModule) { AUDIO_SDK_CLOSE(hModule); hModule = nullptr; }
    }

    /// 上一次 Load 失败的原因, 供 UI 弹窗提示
    const char* LastError() const { return m_lastError; }

private:
    char m_lastError[128] = {};

    inline void LoadFailed(const char* symbol){
        std::snprintf(m_lastError, sizeof(m_lastError),
                      "symbol \"%s\" not found - SDK/header version mismatch?",
                      symbol);
    }
};

#endif  // __cplusplus


// 用完就收, 不污染调用方的宏空间。
// 前四个定义在平台原语头里, 最后一个定义在本文件里 —— 都归这里统一收拾,
// 这样两个平台的头长得一样, 谁也不用记得自己该 undef 什么。
#undef AUDIO_SDK_LOAD
#undef AUDIO_SDK_MODULE_NAME
#undef AUDIO_SDK_OPEN
#undef AUDIO_SDK_SYM
#undef AUDIO_SDK_CLOSE
