/* @Created On : 2026/9/10
   @Author : 孟源
   @note : 音频播放接口(平台无关, PIMPL)
          对外只暴露本头 + audio_sdk.dll, 不含任何 winmm/Windows 类型。
          实现各平台自备: Windows=src/windows/audio_player.cpp, Android=src/android/audio_player.cpp。
*/
#pragma once

#include "audio_sdk/audio_types.h"
#include "audio_sdk/audio_export.h"

#include <cstdint>

class AUDIO_API CAudioPlayer
{
public:
    CAudioPlayer();
    ~CAudioPlayer();
    // 禁止拷贝(内部持有实现指针, 浅拷贝会 double-free)
    CAudioPlayer(const CAudioPlayer&) = delete;
    CAudioPlayer& operator=(const CAudioPlayer&) = delete;

    // 打开并播放 .wav/.aenc(自动分流)。filePath 约定 UTF-8 编码。
    AudioSdk::AudioSdkState PlayWavFile(const char* utf8Path);

    void PausePlay();              // 暂停播放
    void ResumePlay();             // 恢复播放
    void StopPlay();               // 停止播放

    AudioSdk::AudioSdkState Seek(uint32_t posBytes);   // 设置播放位置(字节)
    uint32_t GetPlayPos() const;        // 获取当前播放位置(字节)
    uint32_t GetTotalPos() const;       // 获取总播放长度(字节)
    uint32_t GetPlayPosMs() const;      // 当前播放位置(毫秒)
    uint32_t GetTotalPosMs() const;     // 总时长(毫秒); 未加载文件返回 0

    // 取整个文件的波形(降采样成 CWaveform::kFilePoints 个峰值点)。
    // 在【调用线程】同步回调一次(通常是 UI 线程), 没有并发问题。
    // 未加载文件 / 传 NULL 时不会调用。回调返回后 minmax 缓冲即失效, 需要就自己拷走。
    void BuildWaveform(AudioSdkWaveCallback cb, void* userData);
    bool GetIsPaused() const;           // 获取是否暂停播放
    bool IsPlaying() const;             // 是否正在播放
    bool IsPaused() const;              // 是否暂停

private:
    struct Impl;                    // 前向声明, 实现细节(winmm/AAudio)全在 .cpp
    Impl* m_impl;
};
