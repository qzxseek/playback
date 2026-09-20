/* @Created On : 2026/9/10
   @Author : 孟源
   @note : 音频录制接口(平台无关, PIMPL)
          对外只暴露本头 + audio_sdk.dll, 不含任何 winmm/Windows 类型。
          实现各平台自备: Windows=src/windows/audio_recorder.cpp, Android=src/android/audio_recorder.cpp。
*/
#pragma once

#include "audio_sdk/audio_types.h"   // AudioSdkState
#include "audio_sdk/audio_export.h"

#include <cstdint>

class AUDIO_API CAudioRecorder
{
public:
    CAudioRecorder();
    ~CAudioRecorder();
    // 禁止拷贝(内部持有实现指针, 浅拷贝会 double-free)
    CAudioRecorder(const CAudioRecorder&) = delete;
    CAudioRecorder& operator=(const CAudioRecorder&) = delete;

    AudioSdk::AudioSdkState StartRecording();   // 开始录音
    void PauseResumeRecording();                // 暂停/继续录音
    AudioSdk::AudioSdkState StopRecording();    // 停止录音并落盘(默认 .aenc)

    void SetOutputPath(const char* utf8Path);

    void SetAencEncrypt();                      // 切换加密开关
    bool GetAencEncrypt() const;                // 当前是否加密保存
    bool GetIsPaused() const;                   // 是否暂停
    uint32_t GetRecordedMs() const;             // 已录制时长(毫秒)

    // 注册/取消波形回调(录制中每积累一块就在【音频线程】回调一次)。
    // 传 NULL 取消。回调里只许做"拷贝数据 + PostMessage"这类极快的事,
    // 禁止分配内存/加锁/操作 UI —— 参见 AudioSdkWaveCallback 的说明。
    //
    // 【重要】StopRecording() 会把这个回调清掉(那时设备已静默, 清是安全的)。
    // 所以每次开始录音都要重新注册 —— 不能只注册一次就指望一直有效。
    void SetWaveCallback(AudioSdkWaveCallback cb, void* userData);

private:
    struct Impl;                    // 前向声明, 实现细节(winmm/AAudio)全在 .cpp
    Impl* m_impl;
};
