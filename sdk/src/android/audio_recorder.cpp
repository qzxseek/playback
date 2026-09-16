/* @Created On : 2026/9/10
   @Author : 孟源
   @note : 音频录制实现(Android, PIMPL: AAudio 全部收在 Impl 内, 不泄露到接口头)
           仅在 Android/NDK 下编译(依赖 <aaudio/AAudio.h>, 要求 API 26+)。
           对外接口见 audio_sdk/audio_recorder.h(平台无关)。
*/
#include "audio_sdk/audio_recorder.h"      
#include "audio_sdk/wav_format.h"    
#include "audio_sdk/waveform.h"      

#include <aaudio/AAudio.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// 单声道 16bit: 每帧 2 字节(回调给的 numFrames 是帧数)
static constexpr int kBytesPerFrame = CHANNELS * (BITS_PER_SAMPLE / 8);

// 波形聚合缓冲: 攒够这么多个采样才算一次峰值推给调用方。
// 为什么需要它: AAudio 回调比 winmm 频繁得多(块小、次数多),
// 每来一次回调就推一遍波形会把 UI 淹掉; 攒到与 winmm 一块(100ms)相当再推,
// 两个平台给 UI 的数据节奏就一致了。
static constexpr size_t kAccumSamples = SAMPLE_RATE / 10;   // 100ms 的采样数

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
    // 跨线程读写: UI 线程置位, 音频线程在 OnAudioReady 里读 → 必须原子
    std::atomic<bool> m_isRecording{false};
    bool m_isPaused      = false;
    bool m_isAencEncrypt = true;
    AAudioStream* m_stream = nullptr;
    std::vector<uint8_t> m_vecPcmData;

    // ---- 波形 ----
    // 回调指针用原子: UI 线程注册/注销, 音频线程取快照(音频线程不能加锁)
    std::atomic<AudioSdkWaveCallback> m_waveCb{nullptr};
    std::atomic<void*>                m_waveUser{nullptr};
    // 以下三个缓冲都预先分配 —— 音频线程里只算不分配(分配会导致爆音)
    int16_t m_waveAccum[kAccumSamples] = {};                   // 攒采样的定长缓冲
    size_t  m_waveAccumCount = 0;                              // 已攒采样数
    float   m_waveBuf[CWaveform::kPointsPerBlock * 2] = {};    // 算好的峰值(给回调)
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
    if (p->m_isRecording.load()) return AudioSdk::AudioSdkState::NONE;

    p->m_vecPcmData.clear();
    p->m_waveAccumCount = 0;      // 上一轮的残留采样不带到这一轮
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
    if (!p->m_isRecording.load() || !p->m_stream) return;
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
    if (!p->m_isRecording.load()) return AudioSdk::AudioSdkState::NONE;

    p->m_isRecording = false;    // 先置位: 音频线程此后不再往 UI 推波形
    p->m_isPaused = false;
    if (p->m_stream) {
        AAudioStream_requestStop(p->m_stream);    // 先停回调, 再动数据
        AAudioStream_close(p->m_stream);
        p->m_stream = nullptr;
    }

    // 流已关闭, 不会再有在途回调, 此时注销波形回调是安全的
    p->m_waveCb.store(nullptr, std::memory_order_release);
    p->m_waveUser.store(nullptr, std::memory_order_relaxed);
    p->m_waveAccumCount = 0;

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
    if (!m_isRecording.load(std::memory_order_acquire))
        return AAUDIO_CALLBACK_RESULT_STOP;       // 不该来的回调, 让它停
    const size_t bytes = static_cast<size_t>(numFrames) * kBytesPerFrame;
    auto* pcm = static_cast<const uint8_t*>(audioData);

    // ---- 1) 波形: 先攒够一块, 再算一次峰值推出去 ----
    // 跑在音频线程: 只用预分配的定长缓冲, 不分配内存、不加锁
    if (const AudioSdkWaveCallback cb = m_waveCb.load(std::memory_order_acquire)) {
        const size_t room = sizeof(m_waveAccum) - m_waveAccumCount * sizeof(int16_t);
        const size_t n = bytes < room ? bytes : room;   // 极端情况装不下就只收这么多,
                                                        // 丢的是波形(主录音数据不受影响)
        std::memcpy(reinterpret_cast<uint8_t*>(m_waveAccum)
                        + m_waveAccumCount * sizeof(int16_t),
                    pcm, n);
        m_waveAccumCount += n / sizeof(int16_t);

        if (m_waveAccumCount * sizeof(int16_t) >= sizeof(m_waveAccum)) {
            CWaveform::ComputePeaks(m_waveAccum, sizeof(m_waveAccum),
                                    m_waveBuf, CWaveform::kPointsPerBlock);
            cb(m_waveBuf, CWaveform::kPointsPerBlock,
               m_waveUser.load(std::memory_order_relaxed));
            m_waveAccumCount = 0;
        }
    }

    // ---- 2) 录进内存 ----
    m_vecPcmData.insert(m_vecPcmData.end(), pcm, pcm + bytes);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/**
 * @brief 切换是否加密保存(录制中不生效)
 * @return 切换后的加密状态; 录制中返回 false(不允许切换)
 */
void CAudioRecorder::SetAencEncrypt() {
    Impl* p = m_impl;
    if (p->m_isRecording.load()) return;   // 录制中不许切, 和 Windows 一致
    p->m_isAencEncrypt = !p->m_isAencEncrypt;
}

bool CAudioRecorder::GetAencEncrypt() const { return m_impl->m_isAencEncrypt; }

/**
 * @brief 注册/取消录音波形回调
 * @param cb 回调(传 NULL 取消)
 * @param userData 透传给回调的指针
 * @note 只改两个原子指针, 不加锁 —— 音频线程用"取快照"的方式读。
 *       注销后可能还有一个在途回调正在执行, 调用方要保证 userData
 *       指向的对象存活到"确定没有回调在跑"之后(通常是 StopRecording 返回后)。
 */
void CAudioRecorder::SetWaveCallback(AudioSdkWaveCallback cb, void* userData) {
    Impl* p = m_impl;
    p->m_waveUser.store(userData, std::memory_order_relaxed);   // 先给 userData
    p->m_waveCb.store(cb, std::memory_order_release);           // 再发布回调
}

bool CAudioRecorder::GetIsPaused() const { return m_impl->m_isPaused; }

/**
 * @brief 已录制时长(毫秒) —— 已录字节 ÷ 每秒字节数
 * @return 毫秒
 * @note 录音格式固定(见 audio_types.h), 所以字节率直接用常量算,
 *       与落盘 WAV 头里 FillHeader 写的 byteRate 是同一个表达式。
 */
uint32_t CAudioRecorder::GetRecordedMs() const {
    const uint32_t byteRate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
    if (byteRate == 0) return 0;
    // 先乘后除(乘 1000ULL 避免 32 位溢出), 拿到的才是毫秒
    return static_cast<uint32_t>(m_impl->m_vecPcmData.size() * 1000ULL / byteRate);
}
