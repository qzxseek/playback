#pragma once
#include "audio_sdk/common/audio_types.h"

#include <cstdint>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <aaudio/AAudio.h>
#endif

constexpr int kBytesPerFrame = CHANNELS * (BITS_PER_SAMPLE / 8);         // 每帧字节数

class CAudioRecorder{
public:
    CAudioRecorder();
    ~CAudioRecorder();
    // 录音相关
    AudioSdk::AudioSdkState StartRecording();
    void PauseResumeRecording();
    AudioSdk::AudioSdkState StopRecording();

    // 加解密相关
    void SetAencEncrypt();
    bool GetAencEncrypt();

    bool GetPaused();
    bool GetRecording();
    size_t GetRecordedSize();

private:
#if defined(__ANDROID__)
    // AAudio 回调是C函数，用userData传递this指针
    static aaudio_data_callback_result_t DataCallback(AAudioStream* stream, 
        void* userData, void* audioData, int32_t numFrames);
    static void* ErrorCallback(AAudioStream* stream, void* userData，
        aaudio_error_t error);
    // 录音回调
    aaudio_data_callback_result_t OnAudioReady(AAudioStream* stream,void* audioData,
        int32_t numFrames);
#endif
    std::string m_outputName = "output";
    bool m_isRecording = false;
    bool m_isPaused = false;
    bool m_isAencEncrypt = true;
#if defined(__ANDROID__)
    AAudioStream* m_stream;
#endif
    std::vector<uint8_t> m_vecPcmData;
};

