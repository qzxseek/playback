/* @Created On : 2026/9/12
   @Author : 孟源
   @note : audio_sdk 的统一接口文件 —— 所有平台共用这一份, 由 SDK 提供。
          Windows UI 用它, 后续 Android(以及其他平台)也走它。

          里面三块:
            1) 导出函数声明(extern "C" 名字不修饰 + AUDIO_API 导出标记)
            2) 音频参数与状态码 —— 直接用 SDK 的 audio_types.h, 不抄第二份
            3) 显式加载器 AudioSdkApi(仅 Windows 编译; 其他平台用上面第 1 块)

          为什么声明必须带 AUDIO_API:
            本头被两边同时使用 —— SDK 自己的 src/common/audio_c_api.cpp 也 include 它,
            那边用 AUDIO_API 做函数定义。声明与定义的导出标记必须一致,
            否则 MSVC 报 C2375("重定义; 不同的链接")。
            AUDIO_API 的展开见 audio_export.h:
              AUDIO_SDK_BUILD 已定义(编 SDK 本体) → dllexport
              未定义(UI 等调用方)              → dllimport

          UI 侧看到的是 dllimport 也不要紧: UI 全程只用 GetProcAddress 取地址
          (decltype 是不求值上下文), 不按名字直接调用, 所以不会生成 __imp_ 引用,
          也就不需要链导入库(.lib)。

          各平台怎么用:
            Windows → 显式加载, 全程不链 .lib:
                        AudioSdkApi api;
                        if (!api.Load()) { 看 api.LastError(); }
                        void* player = api.PlayerCreate();
                        api.PlayerPlayFile(player, "test.wav");
                        api.PlayerDestroy(player);
                        api.Unload();
            Android → 函数直接编进 .so, JNI 里按 C 名直接调, 不需要加载器
*/
#pragma once

#include "audio_sdk/audio_export.h"   
#include "audio_sdk/audio_types.h"    

#include <stdint.h>

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

// 切换加密开关(录制中不生效)
AUDIO_API void AudioSdk_RecorderSetAencEncrypt(void* handle);

AUDIO_API int      AudioSdk_RecorderGetAencEncrypt(void* handle);    // 1=加密保存
AUDIO_API int      AudioSdk_RecorderGetIsPaused(void* handle);       // 1=已暂停
AUDIO_API uint32_t AudioSdk_RecorderGetRecordedMs(void* handle);     // 已录时长(毫秒)

AUDIO_API int AudioSdk_IsAencFile(const char* utf8Path);   // 1=加密, 0=否

#ifdef __cplusplus
}
#endif


// ==================== 显式加载器 ==================== 
#if defined(_WIN32) && defined(__cplusplus)

#include <windows.h>
#include <cstdio>       

/**
 * @brief 从 audio_sdk.dll 里取一个函数指针
 * @param pfn 函数指针变量名
 * @param symbol 函数名(在 audio_sdk.dll 里)
 */
#define AUDIO_SDK_LOAD(pfn, symbol)                                                 \
    do {                                                                             \
        (pfn) = reinterpret_cast<decltype(pfn)>(::GetProcAddress(hModule, symbol));   \
        if ((pfn) == nullptr) { LoadFailed(symbol); bOk = false; }                     \
    } while (0)

/**
 * @brief 从 audio_sdk.dll 里取到的全部函数指针。
 *        Load() 失败返回 false, 用 LastError() 看是哪个符号没找到。
 */
struct AudioSdkApi{
    HMODULE hModule = nullptr;          // dll 模块句柄; 为空表示还没加载 / 加载失败

    // —— 播放器 ——
    decltype(&::AudioSdk_PlayerCreate)                     PlayerCreate      = nullptr;
    decltype(&::AudioSdk_PlayerDestroy)               PlayerDestroy     = nullptr;
    decltype(&::AudioSdk_PlayerPlayFile) PlayerPlayFile    = nullptr;
    decltype(&::AudioSdk_PlayerPausePlay)             PlayerPausePlay   = nullptr;
    decltype(&::AudioSdk_PlayerResumePlay)            PlayerResumePlay  = nullptr;
    decltype(&::AudioSdk_PlayerStopPlay)              PlayerStopPlay    = nullptr;
    decltype(&::AudioSdk_PlayerSeek)         PlayerSeek        = nullptr;
    decltype(&::AudioSdk_PlayerGetPlayPos)        PlayerGetPlayPos  = nullptr;
    decltype(&::AudioSdk_PlayerGetTotalPos)       PlayerGetTotalPos = nullptr;
    decltype(&::AudioSdk_PlayerIsPlaying)              PlayerIsPlaying   = nullptr;
    decltype(&::AudioSdk_PlayerGetPlayPosMs)      PlayerGetPlayPosMs  = nullptr;
    decltype(&::AudioSdk_PlayerGetTotalPosMs)     PlayerGetTotalPosMs = nullptr;

    // —— 录音器 ——
    decltype(&::AudioSdk_RecorderCreate)                   RecorderCreate            = nullptr;
    decltype(&::AudioSdk_RecorderDestroy)             RecorderDestroy           = nullptr;
    decltype(&::AudioSdk_RecorderStart)                RecorderStart             = nullptr;
    decltype(&::AudioSdk_RecorderPauseResume)         RecorderPauseResume       = nullptr;
    decltype(&::AudioSdk_RecorderStop)                 RecorderStop              = nullptr;
    decltype(&::AudioSdk_RecorderSetAencEncrypt)      RecorderSetAencEncrypt    = nullptr;
    decltype(&::AudioSdk_RecorderGetAencEncrypt)       RecorderGetAencEncrypt    = nullptr;
    decltype(&::AudioSdk_RecorderGetIsPaused)          RecorderGetIsPaused       = nullptr;
    decltype(&::AudioSdk_RecorderGetRecordedMs)   RecorderGetRecordedMs     = nullptr;
    decltype(&::AudioSdk_IsAencFile)             IsAencFile                = nullptr;

    /**
     * @brief 显式加载: LoadLibrary 打开 dll, GetProcAddress 逐个取函数地址。
     *        全部取到返回 true(幂等, 重复调用不会再加载一遍)。
     * @param dllName dll 文件名或完整路径
     * @return 全部符号都取到返回 true; 任一缺失返回 false 并整体回滚
     */
    bool Load(const wchar_t* dllName = L"audio_sdk.dll"){
        if (hModule) return true;                     // 已经加载过
        hModule = ::LoadLibraryW(dllName);
        if (hModule == nullptr) {
            std::snprintf(m_lastError, sizeof(m_lastError),
                          "LoadLibrary(\"%ls\") failed, GetLastError=%lu",
                          dllName, ::GetLastError());
            return false;
        }

        bool bOk = true;

        // 播放器
        AUDIO_SDK_LOAD(PlayerCreate,      "AudioSdk_PlayerCreate");
        AUDIO_SDK_LOAD(PlayerDestroy,     "AudioSdk_PlayerDestroy");
        AUDIO_SDK_LOAD(PlayerPlayFile,    "AudioSdk_PlayerPlayFile");
        AUDIO_SDK_LOAD(PlayerPausePlay,   "AudioSdk_PlayerPausePlay");
        AUDIO_SDK_LOAD(PlayerResumePlay,  "AudioSdk_PlayerResumePlay");
        AUDIO_SDK_LOAD(PlayerStopPlay,    "AudioSdk_PlayerStopPlay");
        AUDIO_SDK_LOAD(PlayerSeek,        "AudioSdk_PlayerSeek");
        AUDIO_SDK_LOAD(PlayerGetPlayPos,  "AudioSdk_PlayerGetPlayPos");
        AUDIO_SDK_LOAD(PlayerGetTotalPos, "AudioSdk_PlayerGetTotalPos");
        AUDIO_SDK_LOAD(PlayerIsPlaying,   "AudioSdk_PlayerIsPlaying");
        AUDIO_SDK_LOAD(PlayerGetPlayPosMs,  "AudioSdk_PlayerGetPlayPosMs");
        AUDIO_SDK_LOAD(PlayerGetTotalPosMs, "AudioSdk_PlayerGetTotalPosMs");

        // 录音器
        AUDIO_SDK_LOAD(RecorderCreate,            "AudioSdk_RecorderCreate");
        AUDIO_SDK_LOAD(RecorderDestroy,           "AudioSdk_RecorderDestroy");
        AUDIO_SDK_LOAD(RecorderStart,             "AudioSdk_RecorderStart");
        AUDIO_SDK_LOAD(RecorderPauseResume,       "AudioSdk_RecorderPauseResume");
        AUDIO_SDK_LOAD(RecorderStop,              "AudioSdk_RecorderStop");
        AUDIO_SDK_LOAD(RecorderSetAencEncrypt,    "AudioSdk_RecorderSetAencEncrypt");
        AUDIO_SDK_LOAD(RecorderGetAencEncrypt,    "AudioSdk_RecorderGetAencEncrypt");
        AUDIO_SDK_LOAD(RecorderGetIsPaused,       "AudioSdk_RecorderGetIsPaused");
        AUDIO_SDK_LOAD(RecorderGetRecordedMs,     "AudioSdk_RecorderGetRecordedMs");
        AUDIO_SDK_LOAD(IsAencFile,                "AudioSdk_IsAencFile");

        if (!bOk) { Unload(); return false; }          // 缺符号就整体回滚, 别留半套指针
        return true;
    }

    /// 卸载 dll(调用前先销毁由它创建的对象, 否则析构会跳进已卸载的代码)
    void Unload()
    {
        if (hModule) { ::FreeLibrary(hModule); hModule = nullptr; }
    }

    /// 上一次 Load 失败的原因, 供 UI 弹窗提示
    const char* LastError() const { return m_lastError; }

private:
    char m_lastError[128] = {};

    inline void LoadFailed(const char* symbol){
        std::snprintf(m_lastError, sizeof(m_lastError),
                      "GetProcAddress(\"%s\") failed - dll/header version mismatch?",
                      symbol);
    }
};

#undef AUDIO_SDK_LOAD

#endif  // _WIN32 && __cplusplus
