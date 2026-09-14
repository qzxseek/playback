/* @Created On : 2026/9/12
   @Author : 孟源
   @note : audio_sdk 的统一接口文件 —— 所有平台共用这一份。
          Windows UI 用它, 后续 Android(以及其他平台)也走它。

          自包含 —— 不 include SDK 目录里的任何头, 拿到它 + audio_sdk.dll 两个文件就能用。
          里面四块:
            1) 音频参数(录音时长换算用)
            2) 状态码(把接口返回的 int 翻译成提示文字)
            3) 导出函数声明(extern "C", 名字不做 C++ 修饰)
            4) 显式加载器 AudioSdkApi(仅 Windows 编译; 其他平台直接用上面第 3 块)

          各平台怎么用:
            Windows → 显式加载, 全程不链 .lib:
                        AudioSdkApi api;
                        if (!api.Load()) { 看 api.LastError(); }
                        void* player = api.PlayerCreate();
                        api.PlayerPlayFile(player, "test.wav");
                        api.PlayerDestroy(player);
                        api.Unload();
            Android → 函数直接编进 .so, JNI 里按 C 名直接调, 不需要加载器

          第 1、2 块与 SDK 内部的 audio_sdk/audio_types.h 是同一套值 ——
          那边是唯一真源, 改了要同步这里(否则时长换算和错误提示会错位)。
*/
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----------- 音频参数 -----------
#define SAMPLE_RATE     44100
#define BITS_PER_SAMPLE 16
#define CHANNELS        1

// ----------- 状态码 -----------
#ifdef __cplusplus
namespace AudioSdk {
enum class AudioSdkState
{
    NONE,                  // 无错误
    DEVICE_NOT_FOUND,      // 设备无法打开
    DEVICE_BUSY,           // 设备已被占用
    FORMAT_NOT_SUPPORTED,  // 格式不支持
    FILE_OPEN_FAILED,      // 文件打开失败
    FILE_WRITE_FAILED,     // 文件写入失败
    FILE_READ_FAILED,      // 文件读取失败
    INVALID_PARAMETER,     // 无效参数
    OUT_OF_MEMORY,         // 内存不足
    PLATFORM_ERROR,        // 平台错误
    UNKNOWN_ERROR,         // 未知错误
};
}
#endif


// ==================== 导出函数声明 ====================

// ==================== 播放器 ====================

// 创建播放器对象; 成功返回句柄(非空), 失败返回 NULL
void* AudioSdk_PlayerCreate(void);

// 销毁播放器对象(句柄可传 NULL, 安全)
void AudioSdk_PlayerDestroy(void* handle);

// 打开并播放 .wav/.aenc(UTF-8 路径); 返回 AudioSdkState 的序号
int AudioSdk_PlayerPlayFile(void* handle, const char* utf8Path);

void AudioSdk_PlayerPausePlay(void* handle);
void AudioSdk_PlayerResumePlay(void* handle);
void AudioSdk_PlayerStopPlay(void* handle);

// 跳转播放位置(字节); 返回 AudioSdkState 的序号
int AudioSdk_PlayerSeek(void* handle, uint32_t posBytes);

uint32_t AudioSdk_PlayerGetPlayPos(void* handle);    // 已播字节
uint32_t AudioSdk_PlayerGetTotalPos(void* handle);   // 总字节
int      AudioSdk_PlayerIsPlaying(void* handle);     // 1=在播, 0=否

// ==================== 录音器 ====================

// 创建录音器对象; 成功返回句柄(非空), 失败返回 NULL
void* AudioSdk_RecorderCreate(void);

// 销毁录音器对象(句柄可传 NULL, 安全)
void AudioSdk_RecorderDestroy(void* handle);

// 开始录音; 返回 AudioSdkState 的序号
int AudioSdk_RecorderStart(void* handle);

// 暂停/继续录音
void AudioSdk_RecorderPauseResume(void* handle);

// 停止录音并落盘; 返回 AudioSdkState 的序号
int AudioSdk_RecorderStop(void* handle);

// 切换加密开关(录制中不生效)
void AudioSdk_RecorderSetAencEncrypt(void* handle);

int AudioSdk_RecorderGetAencEncrypt(void* handle);       // 1=加密保存
int AudioSdk_RecorderGetIsPaused(void* handle);          // 1=已暂停
size_t AudioSdk_RecorderGetRecordedBytes(void* handle);  // 已录字节

#ifdef __cplusplus
}   
#endif


// ==================== 显式加载器 ==================== 
#if defined(_WIN32) && defined(__cplusplus)

#include <windows.h>
#include <cstdio>       // std::snprintf(填加载失败原因)

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

    // —— 录音器 ——
    decltype(&::AudioSdk_RecorderCreate)                   RecorderCreate            = nullptr;
    decltype(&::AudioSdk_RecorderDestroy)             RecorderDestroy           = nullptr;
    decltype(&::AudioSdk_RecorderStart)                RecorderStart             = nullptr;
    decltype(&::AudioSdk_RecorderPauseResume)         RecorderPauseResume       = nullptr;
    decltype(&::AudioSdk_RecorderStop)                 RecorderStop              = nullptr;
    decltype(&::AudioSdk_RecorderSetAencEncrypt)      RecorderSetAencEncrypt    = nullptr;
    decltype(&::AudioSdk_RecorderGetAencEncrypt)       RecorderGetAencEncrypt    = nullptr;
    decltype(&::AudioSdk_RecorderGetIsPaused)          RecorderGetIsPaused       = nullptr;
    decltype(&::AudioSdk_RecorderGetRecordedBytes)  RecorderGetRecordedBytes  = nullptr;

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

        // 录音器
        AUDIO_SDK_LOAD(RecorderCreate,            "AudioSdk_RecorderCreate");
        AUDIO_SDK_LOAD(RecorderDestroy,           "AudioSdk_RecorderDestroy");
        AUDIO_SDK_LOAD(RecorderStart,             "AudioSdk_RecorderStart");
        AUDIO_SDK_LOAD(RecorderPauseResume,       "AudioSdk_RecorderPauseResume");
        AUDIO_SDK_LOAD(RecorderStop,              "AudioSdk_RecorderStop");
        AUDIO_SDK_LOAD(RecorderSetAencEncrypt,    "AudioSdk_RecorderSetAencEncrypt");
        AUDIO_SDK_LOAD(RecorderGetAencEncrypt,    "AudioSdk_RecorderGetAencEncrypt");
        AUDIO_SDK_LOAD(RecorderGetIsPaused,       "AudioSdk_RecorderGetIsPaused");
        AUDIO_SDK_LOAD(RecorderGetRecordedBytes,  "AudioSdk_RecorderGetRecordedBytes");

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
