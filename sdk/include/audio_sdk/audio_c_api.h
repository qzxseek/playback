/* @Created On : 2026/9/12
   @Author : 孟源
   @note : audio_sdk 的统一接口文件 —— 所有平台共用这一份, 由 SDK 提供。
          Windows UI 用它, Android(以及其他平台)也走它。

          里面三块:
            1) 导出函数声明(extern "C" 名字不修饰 + AUDIO_API 导出标记)
            2) 音频参数与状态码 —— 直接用 SDK 的 audio_types.h, 不抄第二份
            3) 显式加载器 AudioSdkApi —— Windows 走 LoadLibrary, Android 走 dlopen;
               两个平台共用同一个结构体和同一张函数指针表, 用法完全一致

          为什么声明必须带 AUDIO_API:
            本头被两边同时使用 —— SDK 自己的 src/common/audio_c_api.cpp 也 include 它,
            那边用 AUDIO_API 做函数定义。声明与定义的导出标记必须一致,
            否则 MSVC 报 C2375("重定义; 不同的链接")。
            AUDIO_API 的展开见 audio_export.h:
              AUDIO_SDK_BUILD 已定义(编 SDK 本体) → dllexport
              未定义(UI 等调用方)              → dllimport

          调用方看到的是 dllimport 也不要紧: 调用方全程只用加载器取地址
          (decltype 是不求值上下文), 不按名字直接调用, 所以不会生成 __imp_ 引用,
          也就不需要链导入库(.lib)。

          各平台怎么用 —— 写法一样, 只是背后打开的动态库不同:
                        AudioSdkApi api;
                        if (!api.Load()) { 看 api.LastError(); }
                        void* player = api.PlayerCreate();
                        api.PlayerPlayFile(player, "test.wav");
                        api.PlayerDestroy(player);
                        api.Unload();          // 调用前必须先销毁全部 handle
            Windows → LoadLibraryW("audio_sdk.dll") / GetProcAddress
            Android → dlopen("libaudio_sdk.so")     / dlsym
                      前提: SDK 要单独编成 libaudio_sdk.so 随 APK 装进 jniLibs,
                      而且桥自己的 .so 不要链它 —— 链了就成了隐式加载, 白搭。
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


// ==================== 显式加载器 ====================
// 两个平台各一套底层调用, 但结构体名、函数指针表、用法完全一致
// 差别只在"开库 / 取符号 / 关库"三个动作上, 所以把这三步收成宏, 由平台分支各自定义;
// 函数指针表因此只需要写一份 —— 将来 SDK 加接口, 不会漏改另一边。
#if defined(__cplusplus) && (defined(_WIN32) || defined(__ANDROID__))

#include <cstdio>

#if defined(_WIN32)
  #include <windows.h>
  using AudioSdkModuleHandle = HMODULE;              // 动态库句柄
  #define AUDIO_SDK_MODULE_NAME  L"audio_sdk.dll"    // 默认库名(Windows 路径用宽字符)
  #define AUDIO_SDK_OPEN(m, n)   (m) = ::LoadLibraryW(n)
  #define AUDIO_SDK_SYM(m, s)    ::GetProcAddress(m, s)
  #define AUDIO_SDK_CLOSE(m)     ::FreeLibrary(m)
#elif defined(__ANDROID__)
  #include <dlfcn.h>
  using AudioSdkModuleHandle = void*;
  #define AUDIO_SDK_MODULE_NAME  "libaudio_sdk.so"
  // RTLD_LOCAL: 符号不导给别人, 免得和桥自己的符号撞名
  #define AUDIO_SDK_OPEN(m, n)   (m) = ::dlopen(n, RTLD_NOW | RTLD_LOCAL)
  #define AUDIO_SDK_SYM(m, s)    ::dlsym(m, s)
  #define AUDIO_SDK_CLOSE(m)     ::dlclose(m)
#else
  #error "不支持的平台"
#endif

/**
 * @brief 从动态库里取一个函数指针
 * @param pfn 函数指针变量名
 * @param symbol 函数名(在动态库里)
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
    decltype(&::AudioSdk_PlayerBuildWaveform)     PlayerBuildWaveform = nullptr;

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
    decltype(&::AudioSdk_RecorderSetWaveCallback) RecorderSetWaveCallback   = nullptr;
    decltype(&::AudioSdk_IsAencFile)             IsAencFile                = nullptr;

    /**
     * @brief 显式加载: 打开动态库, 逐个取函数地址。
     *        全部取到返回 true(幂等, 重复调用不会再加载一遍)。
     * @param moduleName 库文件名或完整路径
     * @return 全部符号都取到返回 true; 任一缺失返回 false 并整体回滚
     */
#if defined(_WIN32)
    bool Load(const wchar_t* moduleName = AUDIO_SDK_MODULE_NAME){
#elif defined(__ANDROID__)
    bool Load(const char* moduleName = AUDIO_SDK_MODULE_NAME){
#endif
        if (hModule) return true;                     // 已经加载过
        AUDIO_SDK_OPEN(hModule, moduleName);
        if (hModule == nullptr) {
#if defined(_WIN32)
            std::snprintf(m_lastError, sizeof(m_lastError),
                          "LoadLibrary(\"%ls\") failed, GetLastError=%lu",
                          moduleName, ::GetLastError());
#else
            const char* reason = ::dlerror();          // dlopen 失败的原因串
            std::snprintf(m_lastError, sizeof(m_lastError),
                          "dlopen(\"%s\") failed: %s",
                          moduleName, reason ? reason : "unknown");
#endif
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
        AUDIO_SDK_LOAD(PlayerBuildWaveform, "AudioSdk_PlayerBuildWaveform");

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
        AUDIO_SDK_LOAD(RecorderSetWaveCallback,   "AudioSdk_RecorderSetWaveCallback");
        AUDIO_SDK_LOAD(IsAencFile,                "AudioSdk_IsAencFile");

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


#undef AUDIO_SDK_LOAD
#undef AUDIO_SDK_MODULE_NAME
#undef AUDIO_SDK_OPEN
#undef AUDIO_SDK_SYM
#undef AUDIO_SDK_CLOSE

#endif  // __cplusplus && (_WIN32 || __ANDROID__)
