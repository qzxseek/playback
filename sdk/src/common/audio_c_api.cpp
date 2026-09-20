/* @Created On : 2026/9/12
   @Author : 孟源
   @note : SDK 对外导出的 C 接口层 —— 纯转发。
          把 C 函数转成对 C++ 类(CAudioPlayer / CAudioRecorder)的调用。
          本文件平台无关: 不含任何 winmm / AAudio 类型,
          Windows 与 Android 编的是同一份源码。
*/
#include "audio_sdk/audio_c_api.h"
#include "audio_sdk/audio_export.h"
#include "audio_sdk/audio_player.h"
#include "audio_sdk/audio_recorder.h"
#include "audio_sdk/encrypted_format.h" 

#include <cstdint>

// 句柄 <-> 指针: void* 就是 C++ 对象指针
static inline CAudioPlayer*   AsPlayer(void* h)   { return static_cast<CAudioPlayer*>(h); }
static inline CAudioRecorder* AsRecorder(void* h) { return static_cast<CAudioRecorder*>(h); }

extern "C" {

// ==================== 播放器 ====================

// 创建播放器对象; 成功返回句柄(非空), 失败返回 NULL
AUDIO_API void* AudioSdk_PlayerCreate(void) {

    try {
        return new CAudioPlayer();
    } catch (...) {
        return nullptr;     // 调用方本来就把 NULL 当"创建失败"
    }
}

// 销毁播放器对象(句柄可传 NULL, 安全)
AUDIO_API void AudioSdk_PlayerDestroy(void* handle) {
    delete AsPlayer(handle);                      // handle 为 nullptr 也安全
}

AUDIO_API int AudioSdk_IsAencFile(const char* utf8Path) {
    return CEncryptedFormat::IsAencFile(utf8Path) ? 1 : 0;
}

// 打开并播放 .wav/.aenc(UTF-8 路径); 返回 AudioSdkState 的序号
AUDIO_API int AudioSdk_PlayerPlayFile(void* handle, const char* utf8Path) {
    if (handle == nullptr)
        return static_cast<int>(AudioSdk::AudioSdkState::INVALID_PARAMETER);
    return static_cast<int>(AsPlayer(handle)->PlayWavFile(utf8Path));
}

AUDIO_API void AudioSdk_PlayerPausePlay(void* handle) {
    if (handle) AsPlayer(handle)->PausePlay();
}

AUDIO_API void AudioSdk_PlayerResumePlay(void* handle) {
    if (handle) AsPlayer(handle)->ResumePlay();
}

AUDIO_API void AudioSdk_PlayerStopPlay(void* handle) {
    if (handle) AsPlayer(handle)->StopPlay();
}

// 跳转播放位置(字节); 返回 AudioSdkState 的序号
AUDIO_API int AudioSdk_PlayerSeek(void* handle, uint32_t posBytes) {
    if (handle == nullptr)
        return static_cast<int>(AudioSdk::AudioSdkState::INVALID_PARAMETER);
    return static_cast<int>(AsPlayer(handle)->Seek(posBytes));
}

AUDIO_API uint32_t AudioSdk_PlayerGetPlayPos(void* handle) {
    return handle ? AsPlayer(handle)->GetPlayPos() : 0;
}

AUDIO_API uint32_t AudioSdk_PlayerGetTotalPos(void* handle) {
    return handle ? AsPlayer(handle)->GetTotalPos() : 0;
}

// 当前播放位置(毫秒) —— UI 显示 mm:ss.d 直接用, 不用自己读文件头算字节率
AUDIO_API uint32_t AudioSdk_PlayerGetPlayPosMs(void* handle) {
    return handle ? AsPlayer(handle)->GetPlayPosMs() : 0;
}

// 总时长(毫秒); 未加载文件返回 0
AUDIO_API uint32_t AudioSdk_PlayerGetTotalPosMs(void* handle) {
    return handle ? AsPlayer(handle)->GetTotalPosMs() : 0;
}

// 取整个文件的波形(降采样)。在调用线程同步回调一次, 无并发。
AUDIO_API void AudioSdk_PlayerBuildWaveform(void* handle,
                                            AudioSdkWaveCallback cb,
                                            void* userData) {
    if (handle) AsPlayer(handle)->BuildWaveform(cb, userData);
}

// 1=在播, 0=否
AUDIO_API int AudioSdk_PlayerIsPlaying(void* handle) {
    return (handle && AsPlayer(handle)->IsPlaying()) ? 1 : 0;
}

// ==================== 录音器 ====================

// 创建录音器对象; 成功返回句柄(非空), 失败返回 NULL
AUDIO_API void* AudioSdk_RecorderCreate(void) {
    // 同 AudioSdk_PlayerCreate: nothrow 保护不到构造体内的分配
    try {
        return new CAudioRecorder();
    } catch (...) {
        return nullptr;
    }
}

// 销毁录音器对象(句柄可传 NULL, 安全)
AUDIO_API void AudioSdk_RecorderDestroy(void* handle) {
    delete AsRecorder(handle);
}

// 开始录音; 返回 AudioSdkState 的序号
AUDIO_API int AudioSdk_RecorderStart(void* handle) {
    if (handle == nullptr)
        return static_cast<int>(AudioSdk::AudioSdkState::INVALID_PARAMETER);
    return static_cast<int>(AsRecorder(handle)->StartRecording());
}

// 暂停/继续录音
AUDIO_API void AudioSdk_RecorderPauseResume(void* handle) {
    if (handle) AsRecorder(handle)->PauseResumeRecording();
}

// 停止录音并落盘; 返回 AudioSdkState 的序号
AUDIO_API int AudioSdk_RecorderStop(void* handle) {
    if (handle == nullptr)
        return static_cast<int>(AudioSdk::AudioSdkState::INVALID_PARAMETER);
    return static_cast<int>(AsRecorder(handle)->StopRecording());
}

// 切换加密开关(录制中不生效)
AUDIO_API void AudioSdk_RecorderSetAencEncrypt(void* handle) {
    if (handle) AsRecorder(handle)->SetAencEncrypt();
}

AUDIO_API int AudioSdk_RecorderGetAencEncrypt(void* handle) {   // 1=加密保存
    return (handle && AsRecorder(handle)->GetAencEncrypt()) ? 1 : 0;
}

AUDIO_API int AudioSdk_RecorderGetIsPaused(void* handle) {      // 1=已暂停
    return (handle && AsRecorder(handle)->GetIsPaused()) ? 1 : 0;
}

// 已录时长(毫秒) —— UI 显示录音时长直接用
AUDIO_API uint32_t AudioSdk_RecorderGetRecordedMs(void* handle) {
    return handle ? AsRecorder(handle)->GetRecordedMs() : 0;
}

// 注册/取消录音波形回调(cb 传 NULL 取消)。
// 回调跑在音频线程, 只许做"拷贝 + PostMessage", 详见 audio_types.h 的说明。
AUDIO_API void AudioSdk_RecorderSetWaveCallback(void* handle,
                                                AudioSdkWaveCallback cb,
                                                void* userData) {
    if (handle) AsRecorder(handle)->SetWaveCallback(cb, userData);
}

}
