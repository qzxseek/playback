#include "audio_sdk/android/audio_recorder.h"
#include "audio_sdk/common/wav_format.h"        

#include <cstring>

CAudioRecorder::CAudioRecorder() = default;

CAudioRecorder::~CAudioRecorder(){
    if (m_stream){
        AAudioStream_close(&m_stream);
        m_stream = nullptr;
    }
}

/**
 * @brief 开始录音
 * @return AudioSdk::AudioSdkState 
 */
AudioSdk::AudioSdkState CAudioRecorder::StartRecording(){
    if (m_isRecording) return AudioSdk::AudioSdkState::NONE;
    m_isRecording = true;
    m_isPaused = false;
    m_vecPcmData.clear();

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK || !builder)
        return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(builder, SAMPLE_RATE);           
    AAudioStreamBuilder_setChannelCount(builder, CHANNELS);
    AAudioStreamBuilder_setDataCallback(builder, DataCallback, this);
    AAudioStreamBuilder_setErrorCallback(builder, ErrorCallback, this);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);

    result = AAudioStreamBuilder_openStream(builder, &m_stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK){
        if (result == AAUDIO_ERROR_UNAVAILABLE)
            return AudioSdk::AudioSdkState::DEVICE_BUSY;
        return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;
    }

    // 以设备实际给的采样率/声道为准（有的设备不支持44100Hz）
    result = AAudioStream_requestStart(m_stream);
    if (result != AAUDIO_OK){
        AAudioStream_close(&m_stream);
        m_stream = nullptr;
        return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;
    }
    
    m_isRecording = true;
    return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 暂停/继续录音
 * @return AudioSdk::AudioSdkState 
 */
void CAudioRecorder::PauseResumeRecording() {
    if (!m_isRecording || !m_stream) return;
    if (m_isPaused) {
        AAudioStream_requestStart(m_stream);   
        m_isPaused = false;
    } else {
        // 输入流常不支持真正的 pause,用 stop 模拟:暂停期间不回调=不攒数据
        AAudioStream_requestStop(m_stream);
        m_isPaused = true;
    }
}

/**
 * @brief 停止录音
 * @return 播放状态码
 */
AudioSdk::AudioSdkState CAudioRecorder::StopRecording() {
    if (!m_isRecording) return AudioSdk::AudioSdkState::NONE;

    m_isRecording = false;
    m_isPaused = false;
    if (m_stream) {
        AAudioStream_requestStop(m_stream);    // 先停回调,再动数据
        AAudioStream_close(m_stream);
        m_stream = nullptr;
    }

    // 此刻回调已停,不会再写 m_vecPcmData,可直接访问——这就是"先 stop 再落盘"的原因
    std::string outFile = m_outputName + (m_isAencEncrypt ? ".aenc" : ".wav");
    return CWavFormat::SaveWavFile(outFile.c_str(), m_vecPcmData.data(),
        m_vecPcmData.size(),m_isAencEncrypt);          
}

/**
 * @brief AAudio 错误回调:音频线程
 * @param stream  音频流
 * @param userData  用户数据
 * @param error  错误码
 */
void CAudioRecorder::ErrorCallback(AAudioStream*, void* userData, aaudio_result_t error) {
    auto* self = reinterpret_cast<CAudioRecorder*>(userData);
    // 设备被拔 / 出错:把流停掉,避免继续回调。落盘由 StopRecording 兜底。
    if (error == AAUDIO_ERROR_DISCONNECTED && self->m_stream) {
        AAudioStream_close(self->m_stream);
        self->m_stream = nullptr;
    }
}

/**
 * @brief AAudio 数据回调:音频线程
 * @param stream 
 * @param audioData 
 * @param numFrames 
 * @return aaudio_data_callback_result_t 
 */
aaudio_data_callback_result_t CAudioRecorder::OnAudioReady(
        AAudioStream*, void* audioData, int32_t numFrames) {
    if (!m_isRecording)
        return AAUDIO_CALLBACK_RESULT_STOP;                     // 不该来的回调,让它停
    const size_t bytes = static_cast<size_t>(numFrames) * kBytesPerFrame;
    auto* p = static_cast<const uint8_t*>(audioData);
    m_vecPcmData.insert(m_vecPcmData.end(), p, p + bytes);  // 只做内存拷贝
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/**
 * @brief AAudio 回调是 C 自由函数,不能是成员函数,用 userData 把 this 传进去
 * @param stream  音频流
 * @param userData  用户数据
 * @param audioData  音频数据
 * @param numFrames  帧数
 * @return  回调函数
 */
aaudio_data_callback_result_t CAudioRecorder::DataCallback(
        AAudioStream* stream, void* userData, void* audioData, int32_t numFrames) {
    return reinterpret_cast<CAudioRecorder*>(userData)->OnAudioReady(stream, audioData, numFrames);
}

/**
 * @brief 设置/获取是否加密
 * @return bool 
 */
void CAudioRecorder::SetAencEncrypt() {
    if (m_isRecording) return;             // 录制中不许切,和 Windows 一致
    m_isAencEncrypt = !m_isAencEncrypt;
}

bool CAudioRecorder::GetAencEncrypt() const { return m_isAencEncrypt; }

bool CAudioRecorder::GetIsPaused() const { return m_isPaused; }

size_t CAudioRecorder::GetRecordedBytes() const { return m_vecPcmData.size(); }

bool CAudioRecorder::GetPaused() const { return m_isPaused; }
