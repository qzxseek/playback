/* @Created On : 2026/9/10
   @Author : 孟源
   @note : 音频录制实现(Android, PIMPL: AAudio 全部收在 Impl 内, 不泄露到接口头)
           仅在 Android/NDK 下编译(依赖 <aaudio/AAudio.h>, 要求 API 26+)。
           对外接口见 audio_sdk/audio_recorder.h(平台无关)。
*/
#include "audio_sdk/audio_recorder.h"
#include "audio_sdk/wav_format.h"
#include "audio_sdk/waveform.h"
#include "audio_sdk/wave_ring.h"     // CWaveRing: 音频线程写、调用线程读的峰值环形缓冲

#include <aaudio/AAudio.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>          
#include <stdexcept>    
#include <string>
#include <vector>

// 单声道 16bit: 每帧 2 字节(回调给的 numFrames 是帧数)
static constexpr int kBytesPerFrame = CHANNELS * (BITS_PER_SAMPLE / 8);

// 波形聚合缓冲: 攒够这么多个采样才算一次峰值、写进环形缓冲。
// 为什么需要它: AAudio 回调比 winmm 频繁得多(块小、次数多),
// 每来一次回调就推一遍波形会把调用方淹掉; 攒到与 winmm 一块(同样由
// AUDIO_SDK_BLOCK_MS 定)相当再推, 两个平台给调用方的数据节奏就一致了。
static constexpr size_t kAccumSamples = SAMPLE_RATE * AUDIO_SDK_BLOCK_MS / 1000;

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

    std::string m_outputPath = "output";
    // 跨线程读写: UI 线程置位, 音频线程在 OnAudioReady 里读 → 必须原子
    std::atomic<bool> m_isRecording{false};
    bool m_isPaused      = false;
    bool m_isAencEncrypt = true;
    AAudioStream* m_stream = nullptr;
    std::vector<uint8_t> m_vecPcmData;   // 只有音频线程写; 落盘在流关闭后读
    // 已录字节数
    std::atomic<size_t> m_recordedBytes{0};
    // 同样跨线程: 音频线程分配失败时置位, StopRecording 读它决定返回什么。
    // 含义是"录的过程中内存不够过, m_vecPcmData 里的数据是残缺的"。
    std::atomic<bool> m_oom{false};

    // ---- 波形 ----
    // 峰值环形缓冲: 音频线程写(Push), 调用线程读(Pop)。单生产者单消费者, 无锁。
    // 这里就是"把音频线程契约变成结构"的落点 —— 音频线程上跑的全是 SDK 代码,
    CWaveRing m_waveRing;
    // 以下三个缓冲都预先分配 —— 音频线程里只算不分配(分配会导致爆音)
    int16_t m_waveAccum[kAccumSamples] = {};                   // 攒采样的定长缓冲
    size_t  m_waveAccumCount = 0;                              // 已攒采样数
    float   m_waveBuf[CWaveform::kPointsPerBlock * 2] = {};    // 算好的峰值(推给环形缓冲)
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
    p->m_recordedBytes.store(0, std::memory_order_relaxed);   // 计数跟着清零
    p->m_waveAccumCount = 0;      // 上一轮的残留采样不带到这一轮
    // 波形从头开始: 丢掉上一轮没取走的点(此时流还没 start, 音频线程没在跑)
    p->m_waveRing.Reset();
    p->m_oom.store(false, std::memory_order_relaxed);   // 上一轮的 OOM 标记不带进这一轮
    p->m_isPaused = false;

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result == AAUDIO_ERROR_NO_MEMORY)
        return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
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
        // 开流要分配内部缓冲, 这是实际会碰到的 OOM
        if (result == AAUDIO_ERROR_NO_MEMORY)
            return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
        return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;
    }

    p->m_isRecording = true; 
    result = AAudioStream_requestStart(p->m_stream);
    if (result != AAUDIO_OK){
        p->m_isRecording = false;    
        AAudioStream_close(p->m_stream);
        p->m_stream = nullptr;
        // 流已经开成功了, 这里再报 DEVICE_NOT_FOUND 是不实之词
        if (result == AAUDIO_ERROR_NO_MEMORY)
            return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
        return AudioSdk::AudioSdkState::PLATFORM_ERROR;
    }

    return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 暂停/继续录音
 */
void CAudioRecorder::PauseResumeRecording() {
    Impl* p = m_impl;
    if (!p->m_isRecording.load() || !p->m_stream) return;
    // OOM 之后流已经被回调请求停掉了, 再 requestStart 也只会空转
    if (p->m_oom.load()) return;
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

    // 流已关闭, 音频线程不会再碰任何缓冲了
    p->m_waveAccumCount = 0;
    // 环形缓冲里的点不清: 调用方可能还没来得及取最后一块, 留着让它在 Stop 之后
    // 仍能取到尾巴(下一次 StartRecording 会 Reset)

    // 此刻回调已停, 不会再写 m_vecPcmData, 可直接访问——这就是"先 stop 再落盘"的原因
    // 后缀在这里按加密开关补上(路径本身就是 UTF-8, 格式层收的也是 UTF-8)
    std::string outFile = p->m_outputPath + (p->m_isAencEncrypt ? ".aenc" : ".wav");
    const bool oom = p->m_oom.load(std::memory_order_relaxed);
    const AudioSdk::AudioSdkState saved =
        CWavFormat::SaveWavFile(outFile.c_str(), p->m_vecPcmData.data(),
                                p->m_vecPcmData.size(), p->m_isAencEncrypt);

    // 落盘失败优先: 文件根本没写出来, 比"数据不完整"更严重
    if (saved != AudioSdk::AudioSdkState::NONE) return saved;
    // 落盘成功了, 但录的过程中内存不够过 —— 文件是残缺的, 得让调用方知道
    return oom ? AudioSdk::AudioSdkState::OUT_OF_MEMORY : AudioSdk::AudioSdkState::NONE;
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

    // ---- 1) 波形: 先攒够一块, 再算一次峰值写进环形缓冲 ----
    // 跑在音频线程: 只用预分配的定长缓冲, 不分配内存、不加锁、不调用调用方代码。
    // (以前这里外面套了层"注册了回调才算"的条件; 现在无论调用方取不取都往环里写,
    //  反正 Push 只是个拷贝 —— 也不用再操心注册/注销的先后。)
    {
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
            m_waveRing.Push(m_waveBuf, CWaveform::kPointsPerBlock);
            m_waveAccumCount = 0;
        }
    }

    // ---- 2) 录进内存 ----
    // 【音频线程里绝不能抛异常】异常从 AAudio 的数据回调(系统 C 帧)里穿出去是未定义行为。
    // 内存不够就认输: 记下标记并让 AAudio 停流, 已经录到的数据留着 —— m_isRecording
    // 保持为 true, StopRecording 仍会把它落盘, 而不是把用户录的东西丢掉。
    // vector 只会抛这两种, 不必写 catch (...) 把不相干的异常也咽掉。
    try {
        m_vecPcmData.insert(m_vecPcmData.end(), pcm, pcm + bytes);
    } catch (const std::bad_alloc&) {
        m_oom.store(true, std::memory_order_relaxed);
        return AAUDIO_CALLBACK_RESULT_STOP;
    } catch (const std::length_error&) {
        m_oom.store(true, std::memory_order_relaxed);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }
    // 插完再发布计数: 计数只会落后于 vector, 不会超前 —— 它只是"进度提示",
    // 落盘用的是 vector 自己的 size, 不以它为准。单写者用 store 比 fetch_add 便宜。
    m_recordedBytes.store(m_vecPcmData.size(), std::memory_order_relaxed);
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

/**
 * @brief 设置落盘路径(UTF-8, 不含扩展名 —— 后缀按加密开关补)
 * @note 录制中不生效, 与 SetAencEncrypt 同一个道理: 这一轮的"要录到哪"应当开录前就定死。
 *       Android 上这个接口是必需的 —— 默认的相对路径 "output" 会落在不可写的工作目录。
 */
void CAudioRecorder::SetOutputPath(const char* utf8Path) {
    Impl* p = m_impl;
    if (p->m_isRecording.load()) return;   // 录制中不生效
    p->m_outputPath = utf8Path;
}

bool CAudioRecorder::GetAencEncrypt() const { return m_impl->m_isAencEncrypt; }

/**
 * @brief 取走尚未读过的录音波形点(拉模式)
 * @param outMinMax 输出缓冲, 布局 [min0,max0,min1,max1,...], 需 maxPoints*2 个 float
 * @param maxPoints 本次最多取几个点
 * @return 实际取到的点数; 0 = 暂无新数据(不是错误)
 * @note 与音频线程只通过环形缓冲打交道, 不加锁。
 *       不要与 StartRecording 并发调用 —— 开始录音会重置环形缓冲。
 */
int CAudioRecorder::ReadWave(float* outMinMax, int maxPoints) {
    return m_impl->m_waveRing.Pop(outMinMax, maxPoints);
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
    return static_cast<uint32_t>(m_impl->m_recordedBytes.load(std::memory_order_relaxed) * 1000ULL / byteRate);
}
