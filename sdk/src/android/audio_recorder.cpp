/* @Created On : 2026/9/10
   @Author : 孟源
   @note : 音频录制实现(Android, PIMPL: AAudio 全部收在 Impl 内, 不泄露到接口头)
           仅在 Android/NDK 下编译(依赖 <aaudio/AAudio.h>, 要求 API 26+)。
           对外接口见 audio_sdk/audio_recorder.h(平台无关)。
*/
#include "audio_sdk/audio_recorder.h"       // 平台无关接口(PIMPL)
#include "audio_sdk/wav_format.h"    // SaveWavFile(bEncrypt=true 落 .aenc)

#include <aaudio/AAudio.h>
#include <cstdint>
#include <string>
#include <vector>

// 单声道 16bit: 每帧 2 字节(回调给的 numFrames 是帧数)
static constexpr int kBytesPerFrame = CHANNELS * (BITS_PER_SAMPLE / 8);

/**
 * @brief 音频录制实现(Android, PIMPL: AAudio 全部收在 Impl 内, 不泄露到接口头)
 */
struct CAudioRecorder::Impl{
    // AAudio 回调是 C 自由函数, 不能是成员函数; 用 userData 把 this(Impl*) 传进来
    static aaudio_data_callback_result_t DataCallback(AAudioStream* stream, 
        void* userData, void* audioData, int32_t numFrames);
    static void ErrorCallback(AAudioStream* stream, void* userData,
        aaudio_result_t error);

    aaudio_data_callback_result_t OnAudioReady(void* audioData, int32_t numFrames);

    std::string m_outputName = "output";   // 落盘名(不含扩展名)
    bool m_isRecording   = false;
    bool m_isPaused      = false;
    bool m_isAencEncrypt = true;            
    AAudioStream* m_stream = nullptr;      
    std::vector<uint8_t> m_vecPcmData;     
};

CAudioRecorder::CAudioRecorder() : m_impl(new Impl()) {}

CAudioRecorder::~CAudioRecorder(){
    if (m_impl){
        StopRecording();          // 若还在录, 先收尾落盘(内部负责停流+关流)
        delete m_impl;
        m_impl = nullptr;
    }
}

/**
 * @brief 开始录音
 * @return 音频录制状态
 */
AudioSdk::AudioSdkState CAudioRecorder::StartRecording(){
    Impl* p = m_impl;
    if (p->m_isRecording) return AudioSdk::AudioSdkState::NONE;

    p->m_vecPcmData.clear();
    p->m_isPaused = false;

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK || !builder)
        return AudioSdk::AudioSdkState::PLATFORM_ERROR;

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);   // 录音
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);       // 16bit PCM
    AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE);             // 44100
    AAudioStreamBuilder_setChannelCount(builder, CHANNELS);              // 1
    AAudioStreamBuilder_setDataCallback(builder, Impl::DataCallback, p); // userData = Impl*
    AAudioStreamBuilder_setErrorCallback(builder, Impl::ErrorCallback, p);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);

    result = AAudioStreamBuilder_openStream(builder, &p->m_stream);
    AAudioStreamBuilder_delete(builder);        // builder 用完立即删, 别漏
    if (result != AAUDIO_OK){
        if (result == AAUDIO_ERROR_UNAVAILABLE)
            return AudioSdk::AudioSdkState::DEVICE_BUSY;      // 没权限 / 设备被占
        return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;
    }

    result = AAudioStream_requestStart(p->m_stream);
    if (result != AAUDIO_OK){
        AAudioStream_close(p->m_stream);
        p->m_stream = nullptr;
        return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;
    }

    p->m_isRecording = true;      // 开流成功后才置位(失败时不留下"在录"的错乱状态)
    return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 暂停/继续录音
 */
void CAudioRecorder::PauseResumeRecording() {
    Impl* p = m_impl;
    if (!p->m_isRecording || !p->m_stream) return;
    if (p->m_isPaused) {
        AAudioStream_requestStart(p->m_stream);   // 继续采
        p->m_isPaused = false;
    } else {
        // 输入流常不支持真正的 pause, 用 stop 模拟: 暂停期间不回调=不攒数据
        // (和 winmm waveInStop 一样: 暂停那段时间的数据被丢弃)
        AAudioStream_requestStop(p->m_stream);
        p->m_isPaused = true;
    }
}

/**
 * @brief 停止录音并落盘
 * @return 音频录制状态
 */
AudioSdk::AudioSdkState CAudioRecorder::StopRecording() {
    Impl* p = m_impl;
    if (!p->m_isRecording) return AudioSdk::AudioSdkState::NONE;

    p->m_isRecording = false;
    p->m_isPaused = false;
    if (p->m_stream) {
        AAudioStream_requestStop(p->m_stream);    // 先停回调, 再动数据
        AAudioStream_close(p->m_stream);
        p->m_stream = nullptr;
    }

    // 此刻回调已停, 不会再写 m_vecPcmData, 可直接访问——这就是"先 stop 再落盘"的原因
    std::string outFile = p->m_outputName + (p->m_isAencEncrypt ? ".aenc" : ".wav");
    return CWavFormat::SaveWavFile(outFile.c_str(), p->m_vecPcmData.data(),
                                   p->m_vecPcmData.size(), p->m_isAencEncrypt);
}

/**
 * @brief AAudio 数据回调(自由函数) -> 转发到 Impl 成员
 */
aaudio_data_callback_result_t CAudioRecorder::Impl::DataCallback(
        AAudioStream*, void* userData, void* audioData, int32_t numFrames) {
    return reinterpret_cast<Impl*>(userData)->OnAudioReady(audioData, numFrames);
}

/**
 * @brief AAudio 错误回调(自由函数): 音频线程
 */
void CAudioRecorder::Impl::ErrorCallback(
        AAudioStream* stream, void* userData, aaudio_result_t error) {
    auto* p = reinterpret_cast<Impl*>(userData);
    // 设备被拔 / 出错: 只请求停止, 不在回调线程里 close(由 StopRecording 兜底关闭)
    if (error == AAUDIO_ERROR_DISCONNECTED && p->m_stream)
        AAudioStream_requestStop(stream);
}

/**
 * @brief 数据回调实现: 音频线程, 只做内存拷贝
 */
aaudio_data_callback_result_t CAudioRecorder::Impl::OnAudioReady(
        void* audioData, int32_t numFrames) {
    if (!m_isRecording)
        return AAUDIO_CALLBACK_RESULT_STOP;       // 不该来的回调, 让它停
    const size_t bytes = static_cast<size_t>(numFrames) * kBytesPerFrame;
    auto* pcm = static_cast<const uint8_t*>(audioData);
    m_vecPcmData.insert(m_vecPcmData.end(), pcm, pcm + bytes);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/**
 * @brief 切换是否加密保存(录制中不生效)
 * @return 切换后的加密状态; 录制中返回 false(不允许切换)
 */
void CAudioRecorder::SetAencEncrypt() {
    Impl* p = m_impl;
    if (p->m_isRecording) return;       // 录制中不许切, 和 Windows 一致
    p->m_isAencEncrypt = !p->m_isAencEncrypt;
}

bool CAudioRecorder::GetAencEncrypt() const { return m_impl->m_isAencEncrypt; }

bool CAudioRecorder::GetIsPaused() const { return m_impl->m_isPaused; }

size_t CAudioRecorder::GetRecordedBytes() const { return m_impl->m_vecPcmData.size(); }
