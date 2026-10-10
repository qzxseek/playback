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
#include "audio_sdk/logging.h"

#include <aaudio/AAudio.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

static constexpr const char* kTag = "recorder";   // 日志来源标记

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
    CWavStreamWriter m_writer;         // 边录边写; 只有音频线程写, StopRecording 收尾
    // 已录字节数
    std::atomic<size_t> m_recordedBytes{0};
    // 存的是 AudioSdkState 状态码(0 = NONE), 含义是"录到一半就写不进去了, 文件是残缺的"。
    std::atomic<int> m_writeErr{0};
    // 设备被拔/被抢占时由错误回调置位(音频线程)
    std::atomic<bool> m_deviceLost{false};

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
    if (p->m_isRecording.load()) {
        AUDIO_LOG_WARN(kTag, "重复开始录音(已在录, 已录 %u ms), 本次忽略", GetRecordedMs());
        return AudioSdk::AudioSdkState::NONE;
    }

    p->m_recordedBytes.store(0, std::memory_order_relaxed);   // 计数跟着清零
    p->m_waveAccumCount = 0;      // 上一轮的残留采样不带到这一轮
    // 波形从头开始: 丢掉上一轮没取走的点(此时流还没 start, 音频线程没在跑)
    p->m_waveRing.Reset();
    p->m_writeErr.store(0, std::memory_order_relaxed);  // 上一轮的写失败标记不带进这一轮
    p->m_deviceLost.store(false, std::memory_order_relaxed);   // 掉线标记同理
    p->m_isPaused = false;

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result == AAUDIO_ERROR_NO_MEMORY) {
        AUDIO_LOG_ERROR(kTag, "AAudio_createStreamBuilder 内存不足 (AAudio=%d) -> OUT_OF_MEMORY",
                        static_cast<int>(result));
        return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
    }
    if (result != AAUDIO_OK || !builder) {
        AUDIO_LOG_ERROR(kTag, "AAudio_createStreamBuilder 失败 (AAudio=%d) -> PLATFORM_ERROR",
                        static_cast<int>(result));
        return AudioSdk::AudioSdkState::PLATFORM_ERROR;
    }

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
        AUDIO_LOG_ERROR(kTag, "openStream 失败 (AAudio=%d) %uch %uHz %ubit -> %s",
                        static_cast<int>(result), CHANNELS, SAMPLE_RATE, BITS_PER_SAMPLE,
                        result == AAUDIO_ERROR_UNAVAILABLE ? "DEVICE_BUSY"
                      : result == AAUDIO_ERROR_NO_MEMORY  ? "OUT_OF_MEMORY"
                                                          : "DEVICE_NOT_FOUND");
        if (result == AAUDIO_ERROR_UNAVAILABLE)
            return AudioSdk::AudioSdkState::DEVICE_BUSY;      // 没权限 / 设备被占
        // 开流要分配内部缓冲, 这是实际会碰到的 OOM
        if (result == AAUDIO_ERROR_NO_MEMORY)
            return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
        return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;
    }

    p->m_isRecording = true;

    const std::string outFile = p->m_outputPath + (p->m_isAencEncrypt ? ".aenc" : ".wav");
    const AudioSdk::AudioSdkState opened = p->m_writer.Open(outFile.c_str(), p->m_isAencEncrypt);
    if (opened != AudioSdk::AudioSdkState::NONE) {
        AUDIO_LOG_ERROR(kTag, "开录时打不开落盘文件 \"%s\" -> 状态码 %d",
                        outFile.c_str(), static_cast<int>(opened));
        p->m_isRecording = false;
        AAudioStream_close(p->m_stream);
        p->m_stream = nullptr;
        return opened;
    }

    result = AAudioStream_requestStart(p->m_stream);
    if (result != AAUDIO_OK){
        AUDIO_LOG_ERROR(kTag, "requestStart 失败 (AAudio=%d, 流已开成功) -> %s",
                        static_cast<int>(result),
                        result == AAUDIO_ERROR_NO_MEMORY ? "OUT_OF_MEMORY" : "PLATFORM_ERROR");
        p->m_isRecording = false;
        AAudioStream_close(p->m_stream);
        p->m_stream = nullptr;
        p->m_writer.Finish();      // 文件已经建了(空 WAV), 关掉别留着占 fd
        // 流已经开成功了, 这里再报 DEVICE_NOT_FOUND 是不实之词
        if (result == AAUDIO_ERROR_NO_MEMORY)
            return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
        return AudioSdk::AudioSdkState::PLATFORM_ERROR;
    }

    AUDIO_LOG_INFO(kTag, "开始录音 -> \"%s%s\" (%uch %uHz %ubit %s)",
                   p->m_outputPath.c_str(), p->m_isAencEncrypt ? ".aenc" : ".wav",
                   CHANNELS, SAMPLE_RATE, BITS_PER_SAMPLE,
                   p->m_isAencEncrypt ? "加密" : "明文");
    return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 暂停/继续录音
 */
void CAudioRecorder::PauseResumeRecording() {
    Impl* p = m_impl;
    if (!p->m_isRecording.load() || !p->m_stream) return;
    // 写失败之后回调已经请求停流了, 再 requestStart 也只会空转
    if (p->m_writeErr.load(std::memory_order_relaxed) != 0) return;
    if (p->m_isPaused) {
        AAudioStream_requestStart(p->m_stream);   // 继续采
        p->m_isPaused = false;
        AUDIO_LOG_INFO(kTag, "继续录音 (已录 %u ms)", GetRecordedMs());
    } else {
        // 输入流常不支持真正的 pause, 用 stop 模拟: 暂停期间不回调=不攒数据
        // (和 winmm waveInStop 一样: 暂停那段时间的数据被丢弃)
        AAudioStream_requestStop(p->m_stream);
        p->m_isPaused = true;
        AUDIO_LOG_INFO(kTag, "暂停录音 (已录 %u ms)", GetRecordedMs());
    }
}

/**
 * @brief 停止录音并落盘
 * @return 音频录制状态
 */
AudioSdk::AudioSdkState CAudioRecorder::StopRecording() {
    Impl* p = m_impl;
    if (!p->m_isRecording.load()) return AudioSdk::AudioSdkState::NONE;

    // 设备中途掉线(错误回调置的): 数据还在, 但这一轮是被打断的 —— 落完盘要如实报出去
    const bool lost = p->m_deviceLost.exchange(false, std::memory_order_acq_rel);

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

    // 此刻回调已停, 不会再动 writer —— 这就是"先停流再收尾"的原因
    // 后缀在这里按加密开关补上(路径本身就是 UTF-8, 格式层收的也是 UTF-8)
    const std::string outFile = p->m_outputPath + (p->m_isAencEncrypt ? ".aenc" : ".wav");
    const size_t nBytes = static_cast<size_t>(p->m_writer.Written());

    const bool flushed = p->m_writer.Finish();         // 回填真实长度 + 关文件
    // 录制途中的写失败在那一刻就定了, 这里只取一次结果; 取走即清
    const int writeErr = p->m_writeErr.exchange(0, std::memory_order_acq_rel);

    // 写失败优先: 数据在半路就断了, 文件是残缺的, 比"收尾没刷出去"更该先报
    if (writeErr != 0) {
        AUDIO_LOG_ERROR(kTag, "录制途中写失败, \"%s\" 只写到 %zu 字节 -> 状态码 %d",
                        outFile.c_str(), nBytes, writeErr);
        return static_cast<AudioSdk::AudioSdkState>(writeErr);
    }
    // 数据都写进去了, 但收尾回填长度/刷缓冲失败 —— 文件头还是 0 长度, 打不开
    if (!flushed) {
        AUDIO_LOG_ERROR(kTag, "收尾写文件头/刷缓冲失败, \"%s\" (%zu 字节) 是残的 -> FILE_WRITE_FAILED",
                        outFile.c_str(), nBytes);
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
    }

    const double secs = static_cast<double>(nBytes) /
                        (SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8));
    if (lost) {
        // 数据保住了, 但这一轮是被设备掉线打断的 —— 如实报, 别让调用方以为录全了
        AUDIO_LOG_WARN(kTag, "停止录音(设备中途掉线, 数据不完整) -> \"%s\" (%zu 字节, %.2fs)",
                       outFile.c_str(), nBytes, secs);
        return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;
    }
    AUDIO_LOG_INFO(kTag, "停止录音 -> \"%s\" (%zu 字节, %.2fs)", outFile.c_str(), nBytes, secs);
    return AudioSdk::AudioSdkState::NONE;
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
    if (error == AAUDIO_ERROR_DISCONNECTED && p->m_stream){
        // 只置标记, 别的不做 —— 收尾全部留给 StopRecording
        p->m_deviceLost.store(true, std::memory_order_release);
        AAudioStream_requestStop(stream);
    }
}

/**
 * @brief 数据回调实现: 音频线程, 算波形 + 把这块 PCM 追加进文件
 * @param audioData 音频数据指针
 * @param numFrames 帧数
 * @return aaudio_data_callback_result_t 回调结果
 */
aaudio_data_callback_result_t CAudioRecorder::Impl::OnAudioReady(
        void* audioData, int32_t numFrames) {
    if (!m_isRecording.load(std::memory_order_acquire))
        return AAUDIO_CALLBACK_RESULT_STOP;       // 不该来的回调, 让它停
    const size_t bytes = static_cast<size_t>(numFrames) * kBytesPerFrame;
    auto* pcm = static_cast<const uint8_t*>(audioData);

    // 波形: 先攒够一块, 再算一次峰值写进环形缓冲
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

    // 边录边写盘, 写不进说明盘满/设备出错，可看日志
    auto ret = m_writer.Write(audioData, bytes);
    if (ret != AudioSdk::AudioSdkState::NONE) {
        int iExpected = 0;
        m_writeErr.compare_exchange_strong(iExpected,
            static_cast<int>(ret),
            std::memory_order_relaxed);
        return AAUDIO_CALLBACK_RESULT_STOP;
    }
    m_recordedBytes.store(m_writer.Written(), std::memory_order_relaxed);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/**
 * @brief 切换是否加密保存(录制中不生效)
 * @return 切换后的加密状态; 录制中返回 false(不允许切换)
 */
void CAudioRecorder::SetAencEncrypt() {
    Impl* p = m_impl;
    if (p->m_isRecording.load()) {         // 录制中不许切, 和 Windows 一致
        AUDIO_LOG_WARN(kTag, "录制中调 SetAencEncrypt, 不生效");
        return;
    }
    p->m_isAencEncrypt = !p->m_isAencEncrypt;
    AUDIO_LOG_INFO(kTag, "加密开关 -> %s", p->m_isAencEncrypt ? "开(.aenc)" : "关(.wav)");
}

/**
 * @brief 设置落盘路径(UTF-8, 不含扩展名 —— 后缀按加密开关补)
 * @note 录制中不生效, 与 SetAencEncrypt 同一个道理: 这一轮的"要录到哪"应当开录前就定死。
 *       Android 上这个接口是必需的 —— 默认的相对路径 "output" 会落在不可写的工作目录。
 */
void CAudioRecorder::SetOutputPath(const char* utf8Path) {
    Impl* p = m_impl;
    if (p->m_isRecording.load()) {         // 录制中不生效
        AUDIO_LOG_WARN(kTag, "录制中调 SetOutputPath(\"%s\"), 不生效", utf8Path ? utf8Path : "");
        return;
    }
    p->m_outputPath = utf8Path;
    AUDIO_LOG_INFO(kTag, "落盘路径 -> \"%s\"", utf8Path ? utf8Path : "");
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
 * @brief 录制途中的写失败状态码(NONE = 没失败)
 * @note 与 Windows 侧同一套语义(见 audio_recorder.h): 只读标记, 收尾仍归
 *       调用方的 StopRecording —— 这里不能顺手把状态改成"没在录"
 */
AudioSdk::AudioSdkState CAudioRecorder::GetWriteError() const {
    return static_cast<AudioSdk::AudioSdkState>(
        m_impl->m_writeErr.load(std::memory_order_relaxed));
}

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
