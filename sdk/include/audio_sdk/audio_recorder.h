/* @Created On : 2026/9/10
   @Author : 孟源
   @note : 音频录制接口(平台无关, PIMPL)
          对外只暴露本头 + audio_sdk.dll, 不含任何 winmm/Windows 类型。
          实现各平台自备: Windows=src/windows/audio_recorder.cpp, Android=src/android/audio_recorder.cpp。
*/
#pragma once

#include "audio_sdk/audio_types.h"   // AudioSdkState
#include "audio_sdk/audio_export.h"

#include <cstddef>

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

    void SetAencEncrypt();                      // 切换加密开关
    bool GetAencEncrypt() const;                // 当前是否加密保存
    bool GetIsPaused() const;                   // 是否暂停
    size_t GetRecordedBytes() const;            // 已录制字节数

private:
    struct Impl;                    // 前向声明, 实现细节(winmm/AAudio)全在 .cpp
    Impl* m_impl;
};
