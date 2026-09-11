#pragma once

#include <cstdint>
#include <cstddef>

#pragma once

// SDK 接口状态
namespace AudioSdk{
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

class CAudioPlayer
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
    bool GetIsPaused() const;           // 获取是否暂停播放
    bool IsPlaying() const;             // 是否正在播放
    bool IsPaused() const;              // 是否暂停

private:
    struct Impl;                    // 前向声明, 实现细节(winmm/AAudio)全在 .cpp
    Impl* m_impl;
};

class CAudioRecorder
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
