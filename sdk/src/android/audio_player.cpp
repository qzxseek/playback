/* @Created On : 2026/9/10
   @Author : 孟源
   @note : 音频播放实现(Android, PIMPL: AAudio 全部收在 Impl 内, 不泄露到接口头)
           仅在 Android/NDK 下编译(依赖 <aaudio/AAudio.h>, 要求 API 26+)。
           流式读取: 开一条生产者线程按块读进 CByteRing, AAudio 回调只从环里拷 
           对外接口见 audio_sdk/audio_player.h(平台无关), 与 Windows 侧同名同签名。
*/
#include "audio_sdk/audio_player.h"
#include "audio_sdk/wav_validate.h"
#include "audio_sdk/wav_format.h"
#include "audio_sdk/encrypted_format.h"
#include "audio_sdk/ogg_format.h"    
#include "audio_sdk/waveform.h"
#include "audio_sdk/byte_ring.h"
#include "audio_sdk/logging.h"

#include <aaudio/AAudio.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>    // std::system_error: std::thread 起不来时抛的
#include <string>
#include <thread>
#include <vector>

static constexpr const char* kTag = "player";   // 日志来源标记

// 生产者空转时的睡眠间隔
static constexpr int kIdleMs = 2;

/**
 * @brief 音频播放实现(Android, PIMPL: AAudio 全部收在 Impl 内, 不泄露到接口头)
 */
struct CAudioPlayer::Impl
{
    // AAudio 回调是 C 自由函数, 用 userData 把 this(Impl*) 传进来
    static aaudio_data_callback_result_t DataCallback(
        AAudioStream* stream, void* userData, void* audioData, int32_t numFrames);
    static void ErrorCallback(
        AAudioStream* stream, void* userData, aaudio_result_t error);

    aaudio_data_callback_result_t OnAudioReady(void* audioData, int32_t numFrames);

    void CleanUpStream();             // 停生产者 + 停流 + 关流 + 清状态
    void ReadLoop();                  // 生产者线程: 把文件里的 PCM 读进环

    // 播放期间文件常开, 只由生产者线程碰(设备回调不碰文件)。
    std::ifstream m_file;
    std::string   m_path;             // 原路径: BuildWaveform 要另开一份流
    size_t m_payloadOffset = 0;       // data 块在文件里的起始偏移
    bool m_encrypted = false;       // 密文容器: 每块要按载荷内偏移解密
    // PCM 总字节数(载荷长度)。原子是因为: 生产者线程在解码提前结束时会把它收到
    // 实际位置, 而 UI 线程同时在读它画进度条 —— 不原子就是数据竞争。
    std::atomic<size_t> m_dataSize{0};

    // 压缩流(.ogg/.aogg)的解码器。Start 之后只有生产者线程碰它 —— Open 在起线程
    // 之前、Close 在 join 之后、Read 和 SeekTo 都在 ReadLoop 里, 所以不用锁。
    // (Windows 那边 Seek 直接从调用线程摁解码器, 才需要拿 m_cs 把生产者挡开。)
    COggStreamReader  m_oggReader;
    std::atomic<bool> m_bOgg{false};  // 本次播的是不是 Ogg(压缩流)

    CByteRing m_ring;
    std::thread m_reader;
    std::atomic<bool> m_stopReq{false};

    // 回调自然播完置位(true 表示已喂完并返回 STOP), 供 IsPlaying() 如实上报
    std::atomic<bool> m_reachedEnd{false};
    int32_t  m_channels = CHANNELS;
    int32_t  m_bytesPerFrame = CHANNELS * (BITS_PER_SAMPLE / 8);
    uint32_t m_byteRate = 0;          // 每秒字节数(从文件头取, 供字节→毫秒换算)

    bool m_isPlaying = false;
    bool m_isPaused  = false;
    AAudioStream* m_stream = nullptr; // 必须初始化, 否则析构判断读野指针
};

// ============ 对外接口: 转发生命周期与方法 ============
CAudioPlayer::CAudioPlayer() : m_impl(new Impl()) {}

CAudioPlayer::~CAudioPlayer(){
    if (m_impl){
        m_impl->CleanUpStream();
        delete m_impl;
        m_impl = nullptr;
    }
}

/**
   @brief : 校验头文件、打开 AAudio 输出流、开始播放
   @param : utf8Path - WAV/.aenc 文件路径(UTF-8)
   @return : 音频设备打开状态
*/
AudioSdk::AudioSdkState CAudioPlayer::PlayWavFile(const char* utf8Path){
    if (!utf8Path) {
        AUDIO_LOG_ERROR(kTag, "PlayWavFile 路径为 NULL -> INVALID_PARAMETER");
        return AudioSdk::AudioSdkState::INVALID_PARAMETER;
    }

    // 先清掉上一次(若还在播)
    StopPlay();
    Impl* p = m_impl;
    p->m_dataSize = 0;
    p->m_path.clear();

    // 文件常开到播完为止(Android 路径就是 UTF-8, 直接开)
    std::ifstream& file = p->m_file;
    file.open(utf8Path, std::ios::in | std::ios::binary);
    if (!file.is_open()) {
        AUDIO_LOG_ERROR(kTag, "打开失败 path=\"%s\" -> FILE_OPEN_FAILED", utf8Path);
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;
    }

    file.seekg(0, std::ios::end);
    const std::streamsize lFileSize = file.tellg();
    if (lFileSize < 4) {
        AUDIO_LOG_ERROR(kTag, "文件只有 %lld 字节, 魔数都放不下 path=\"%s\" -> FORMAT_NOT_SUPPORTED",
                        static_cast<long long>(lFileSize), utf8Path);
        return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;   // 文件太小, 连魔数都放不下
    }

    const size_t prefixSize = lFileSize < 64 * 1024
                            ? static_cast<size_t>(lFileSize) : size_t{64 * 1024};
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> vecBuf(prefixSize);
    if (!file.read(reinterpret_cast<char*>(vecBuf.data()), static_cast<std::streamsize>(prefixSize))) {
        AUDIO_LOG_ERROR(kTag, "读取失败 path=\"%s\" -> FILE_READ_FAILED", utf8Path);
        return AudioSdk::AudioSdkState::FILE_READ_FAILED;
    }
    const size_t fileSize = static_cast<size_t>(lFileSize);

    WavValidate validator;
    const WavHeader* pHdr = nullptr;
    bool bEncrypted  = false;
    bool bOgg        = false;
    size_t payloadOffset = 0;

    if (COggFormat::IsOggData(vecBuf.data(), vecBuf.size()) ||
        COggFormat::IsAoggData(vecBuf.data(), vecBuf.size())){
        const AudioSdk::AudioSdkState ost = p->m_oggReader.Open(utf8Path);
        if (ost != AudioSdk::AudioSdkState::NONE) {
            AUDIO_LOG_ERROR(kTag, "Ogg 打不开 path=\"%s\" -> 状态码 %d", utf8Path, static_cast<int>(ost));
            return ost;                        // 不是可解的 Vorbis 流
        }
        bOgg = true;
    }
    else if (CEncryptedFormat::IsAencData(vecBuf.data(), vecBuf.size())){
        if (vecBuf.size() < CEncryptedFormat::kAencPrefixSize + sizeof(WavHeader)) {
            AUDIO_LOG_ERROR(kTag, "AENC 容器残缺(%zu 字节, 装不下前缀+头) path=\"%s\" -> FORMAT_NOT_SUPPORTED",
                            vecBuf.size(), utf8Path);
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        }
        // 剥掉 6 字节前缀后校验; 内部 WAV 头自洽(riffSize 等按 44 头算)。
        // 总长也要跟着减 6: 头里的 riffSize 描述的是内层 WAV, 不含容器前缀
        if (!validator.Validate(vecBuf.data() + CEncryptedFormat::kAencPrefixSize,
                                vecBuf.size() - CEncryptedFormat::kAencPrefixSize,
                                fileSize - CEncryptedFormat::kAencPrefixSize)) {
            AUDIO_LOG_ERROR(kTag, "AENC 内层 WAV 头不合法 path=\"%s\" -> FORMAT_NOT_SUPPORTED", utf8Path);
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        }
        pHdr          = &validator.Header();
        bEncrypted    = true;
        // 数据偏移由校验器给出: 标准排布是 44, fmt 后夹了 LIST 等子块时是真实位置
        payloadOffset = CEncryptedFormat::kAencPrefixSize + validator.GetDataOffset();
    }
    else if (vecBuf.size() >= sizeof(WavHeader) &&
                std::memcmp(vecBuf.data(), "RIFF", 4) == 0){
        if (!validator.Validate(vecBuf.data(), vecBuf.size(), fileSize)) {
            AUDIO_LOG_ERROR(kTag, "WAV 头校验不过 path=\"%s\" -> FORMAT_NOT_SUPPORTED", utf8Path);
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        }
        pHdr          = &validator.Header();
        payloadOffset = validator.GetDataOffset();
    }
    else {
        AUDIO_LOG_ERROR(kTag, "既非 Ogg 也非 AENC 也非 RIFF path=\"%s\" -> FORMAT_NOT_SUPPORTED", utf8Path);
        return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;   // 不是认识的音频格式
    }

    p->m_path = utf8Path;
    p->m_bOgg.store(bOgg, std::memory_order_relaxed);

    if (bOgg){
        p->m_channels      = p->m_oggReader.GetChannels();
        p->m_bytesPerFrame = p->m_oggReader.GetBlockAlign();
        p->m_byteRate      = static_cast<uint32_t>(p->m_oggReader.GetSampleRate()) * p->m_bytesPerFrame;
        p->m_encrypted     = false;    // 解密在解码器内部做, 这里再解一遍就是解回密文
        p->m_payloadOffset = 0;        // 压缩流用不上文件偏移, 解码器自己记位置
        p->m_dataSize.store(p->m_oggReader.GetTotalBytes(), std::memory_order_relaxed);
    }
    else {
        p->m_payloadOffset = payloadOffset;
        p->m_encrypted     = bEncrypted;
        p->m_dataSize.store(validator.GetDataSize(), std::memory_order_relaxed);
        p->m_channels = pHdr->numChannels ? pHdr->numChannels : CHANNELS;
        p->m_bytesPerFrame = p->m_channels * (pHdr->bitsPerSample / 8);
        p->m_byteRate = pHdr->byteRate;   // 供 GetPlayPosMs/GetTotalPosMs 换算
    }

    // Ogg 的参数从解码器取, WAV 的从文件头取 —— 下面起流要用
    const int32_t nRate = bOgg ? p->m_oggReader.GetSampleRate() : pHdr->sampleRate;
    const int32_t nBits = bOgg ? p->m_oggReader.GetBitsPerSample() : pHdr->bitsPerSample;

    p->m_ring.Reset(0);                                  // 流式游标从头开始
    p->m_reachedEnd.store(false, std::memory_order_release);

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK || !builder){
        AUDIO_LOG_ERROR(kTag, "AAudio_createStreamBuilder 失败 (AAudio=%d) -> PLATFORM_ERROR",
                        static_cast<int>(result));
        p->CleanUpStream();
        return AudioSdk::AudioSdkState::PLATFORM_ERROR;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);   // 播放
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);       // 16bit PCM
    AAudioStreamBuilder_setSampleRate(builder, nRate);                   // 用文件里的采样率
    AAudioStreamBuilder_setChannelCount(builder, p->m_channels);
    AAudioStreamBuilder_setDataCallback(builder, Impl::DataCallback, p); // userData = Impl*
    AAudioStreamBuilder_setErrorCallback(builder, Impl::ErrorCallback, p);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);

    result = AAudioStreamBuilder_openStream(builder, &p->m_stream);
    AAudioStreamBuilder_delete(builder);        // builder 用完立即删, 别漏
    // 原始 aaudio_result_t 一定要记: 枚举把平台错误压扁了, 光看 DEVICE_NOT_FOUND
    // 不知道底下是 AAUDIO_ERROR_UNAVAILABLE(-899) 还是别的, 排查时差很远
    if (result != AAUDIO_OK){
        AUDIO_LOG_ERROR(kTag, "AAudioStreamBuilder_openStream 失败 (AAudio=%d) %uch %uHz %ubit -> %s",
                        static_cast<int>(result), p->m_channels, nRate, nBits,
                        result == AAUDIO_ERROR_INVALID_FORMAT ? "FORMAT_NOT_SUPPORTED"
                      : result == AAUDIO_ERROR_UNAVAILABLE    ? "DEVICE_BUSY"
                                                              : "DEVICE_NOT_FOUND");
        p->CleanUpStream();
        if (result == AAUDIO_ERROR_INVALID_FORMAT)
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        if (result == AAUDIO_ERROR_UNAVAILABLE)
            return AudioSdk::AudioSdkState::DEVICE_BUSY;
        return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;
    }

    // 先起生产者再开流
    p->m_stopReq.store(false, std::memory_order_release);
    try {
        p->m_reader = std::thread([p] { p->ReadLoop(); });
    } catch (const std::system_error&) {
        AUDIO_LOG_ERROR(kTag, "起播放读取线程失败 -> UNKNOWN_ERROR");
        p->CleanUpStream();
        return AudioSdk::AudioSdkState::UNKNOWN_ERROR;
    }

    result = AAudioStream_requestStart(p->m_stream);
    if (result != AAUDIO_OK){
        AUDIO_LOG_ERROR(kTag, "AAudioStream_requestStart 失败 (AAudio=%d, 流已开成功) -> DEVICE_NOT_FOUND",
                        static_cast<int>(result));
        p->CleanUpStream();
        return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;
    }

    p->m_isPlaying = true;      // 开流成功后才置位
    p->m_isPaused  = false;

    // 成功: 记"播的是哪个文件 + 什么格式"。要回答的是"当时放的是不是我以为的那个" ——
    // 路径给这个答案, 格式和时长给"内容有没有被截断/是不是变样了"
    AUDIO_LOG_INFO(kTag, "开始播放 path=\"%s\" %s %uch %uHz %ubit %.2fs (%zu 字节)",
                   utf8Path, bOgg ? "OGG" : (bEncrypted ? "加密" : "明文"),
                   p->m_channels, nRate, nBits,
                   p->m_byteRate ? static_cast<double>(p->m_dataSize.load()) / p->m_byteRate : 0.0,
                   p->m_dataSize.load());
    return AudioSdk::AudioSdkState::NONE;
}

/**
   @brief : AAudio 数据回调(自由函数) -> 转发到 Impl 成员
*/
aaudio_data_callback_result_t CAudioPlayer::Impl::DataCallback(
        AAudioStream*, void* userData, void* audioData, int32_t numFrames) {
    return reinterpret_cast<Impl*>(userData)->OnAudioReady(audioData, numFrames);
}

/**
   @brief : AAudio 错误回调(自由函数): 音频线程
*/
void CAudioPlayer::Impl::ErrorCallback(
        AAudioStream* stream, void* userData, aaudio_result_t error) {
    if (error == AAUDIO_ERROR_DISCONNECTED && stream)
        AAudioStream_requestStop(stream);
}

/**
   @brief : 从环里取数据喂设备(音频线程)
   @param audioData : 设备要的 PCM 数据
   @param numFrames : 设备本次要的帧数
*/
aaudio_data_callback_result_t CAudioPlayer::Impl::OnAudioReady(
        void* audioData, int32_t numFrames) {
    const size_t want = static_cast<size_t>(numFrames) *
                        static_cast<size_t>(m_bytesPerFrame);   // 设备本次要的字节数
    auto* out = static_cast<uint8_t*>(audioData);

    // 顺序要紧: 先 acquire 读 head —— 它同时把 base/tail 的值也发布过来
    // (Reset 是先写这两支、再用 release 落 head 的)。反过来读会拿到过时的 base,
    // 屏幕上就是"Seek 完了却按老位置判没货"。
    const size_t head = m_ring.Head();
    const size_t base = m_ring.Base();
    const size_t pos  = m_ring.Tail();

    if (pos >= m_dataSize.load(std::memory_order_relaxed)){   // 已喂完
        std::memset(out, 0, want);      // 尾部补静音
        m_reachedEnd.store(true, std::memory_order_release);  // 通知 UI: 自然播完了
        return AAUDIO_CALLBACK_RESULT_STOP;               // 让 AAudio 自动停流
    }

    // 环里没有它要的那一段: 刚开播、Seek 刚改过位置、或者生产者一时没跟上, 补静音继续
    if (pos < base || pos >= head){
        std::memset(out, 0, want);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    size_t n = head - pos;                               // 与 pos/head 同源, 不会越界
    if (n > want) n = want;
    m_ring.ReadAt(pos, out, n);

    // CAS 推进游标: 期间若 Seek 已经改过读位置, 这次 CAS 失败即放弃这一块
    m_ring.AdvanceTail(pos, pos + n);

    if (n < want)
        std::memset(out + n, 0, want - n);               // 末尾不足补静音

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/**
   @brief : 生产者线程 —— 把文件里的 PCM 按块读进环
   @note 循环里三件事: 消费者跑到环外了就重置到它要的位置; 环满/读完就歇一下;
         否则读一块、解密、入环
*/
void CAudioPlayer::Impl::ReadLoop(){
    std::vector<uint8_t> chunk(64 * 1024);

    while (!m_stopReq.load(std::memory_order_relaxed)) {
        const size_t want = m_ring.Tail();
        const size_t head = m_ring.Head();
        const size_t base = m_ring.Base();
        if (want < base || want > head) {
            // 读游标被甩到缓冲窗口之外, 要从 want 重开一轮
            if (m_bOgg.load(std::memory_order_relaxed))
                m_oggReader.SeekTo(want);
            m_ring.Reset(want);
            continue;
        }

        if (head >= m_dataSize.load(std::memory_order_relaxed)) {   // 文件已经读完, 让环慢慢被消费掉
            std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs));
            continue;
        }

        const size_t space = m_ring.Space();
        if (space == 0) {                    // 环满了: 等设备把数据取走
            std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs));
            continue;
        }

        size_t n = m_dataSize.load(std::memory_order_relaxed) - head;
        if (n > chunk.size()) n = chunk.size();
        if (n > space) n = space;

        if (m_bOgg.load(std::memory_order_relaxed)) {
            // 压缩流: 顺序解, 解码器自己记着读到哪了(跟 head 同步推进)
            n = m_oggReader.Read(chunk.data(), n);
            if (n == 0) {
                // 解码器提前给干净了(文件被截断 / 流损坏)
                AUDIO_LOG_WARN(kTag, "解码提前结束于 %zu / %zu 字节, 按实际长度收尾",
                               head, m_dataSize.load(std::memory_order_relaxed));
                m_dataSize.store(head, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs));
                continue;
            }
            m_ring.Write(chunk.data(), n);
            continue;
        }

        // 明文/密文 WAV: 按载荷内偏移直接读文件
        // 每块都 seekg: 读满 64KB(约 0.74 秒音频)才做一次, 开销可以忽略;
        m_file.clear();
        m_file.seekg(static_cast<std::streamoff>(m_payloadOffset + head));
        if (!m_file.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(n))) {
            // 文件被截断/被别的程序截走
            m_file.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs * 10));
            continue;
        }
        if (m_encrypted)
            CEncryptedFormat::XorCrypt(chunk.data(), n, head);   // 密钥相位跟着载荷内偏移走

        m_ring.Write(chunk.data(), n);
    }
}

/**
   @brief : 停生产者 + 停流 + 关流 + 清状态(StopPlay 与析构共用)
   @note 可重入: m_reader 一旦 join 过就不再 joinable, 第二次调用直接跳过。
*/
void CAudioPlayer::Impl::CleanUpStream(){
    m_isPlaying = false;
    m_isPaused  = false;
    m_reachedEnd.store(false, std::memory_order_release);

    // 先停生产者再关流
    m_stopReq.store(true, std::memory_order_release);
    if (m_reader.joinable())
        m_reader.join();

    if (m_stream){
        AAudioStream_requestStop(m_stream);
        AAudioStream_close(m_stream);     // close() 会阻塞到回调线程退出
        m_stream = nullptr;
    }

    m_ring.Reset(0);                      // 生产者已停, 这里动环没有竞争
    if (m_file.is_open())
        m_file.close();                   // 路径留着: BuildWaveform 要按它另开一份流
    m_oggReader.Close();                  // 生产者已 join, 之后没人再碰解码器
    m_bOgg.store(false, std::memory_order_relaxed);
    m_dataSize = 0;
    m_payloadOffset = 0;
    m_encrypted = false;
    }

    void CAudioPlayer::PausePlay(){             // 暂停播放
    Impl* p = m_impl;
    if (!p->m_isPlaying || p->m_isPaused || !p->m_stream) return;
    // 输出流支持真正的 pause: 保留流位置, Resume 后从原处继续
    AAudioStream_requestPause(p->m_stream);
    p->m_isPaused = true;
    AUDIO_LOG_INFO(kTag, "暂停于 %u ms", GetPlayPosMs());
}

void CAudioPlayer::ResumePlay(){             // 继续播放
    Impl* p = m_impl;
    if (!p->m_isPlaying || !p->m_isPaused || !p->m_stream) return;
    AAudioStream_requestStart(p->m_stream);   // 读游标还在原处(环里的数据也还在), 接着喂
    p->m_isPaused = false;
    AUDIO_LOG_INFO(kTag, "继续于 %u ms", GetPlayPosMs());
}

/**
   @brief : 停止播放
*/
void CAudioPlayer::StopPlay(){
    if (m_impl->m_isPlaying)
        AUDIO_LOG_INFO(kTag, "停止于 %u ms / 共 %u ms", GetPlayPosMs(), GetTotalPosMs());
    m_impl->CleanUpStream();
}

/**
   @brief : 跳转播放位置(改游标 + 冲刷设备已排队数据)
   @param : posBytes - 跳转位置, 单位字节
   @return : 播放状态
*/
AudioSdk::AudioSdkState CAudioPlayer::Seek(uint32_t posBytes){
    Impl* p = m_impl;
    if (!p->m_isPlaying) {
        AUDIO_LOG_WARN(kTag, "Seek(%u) 但没在播 -> INVALID_PARAMETER", posBytes);
        return AudioSdk::AudioSdkState::INVALID_PARAMETER;
    }

    if (p->m_bytesPerFrame > 0)
        posBytes -= posBytes % static_cast<uint32_t>(p->m_bytesPerFrame);   // 帧对齐
    if (posBytes > p->m_dataSize.load(std::memory_order_relaxed))
        posBytes = static_cast<uint32_t>(p->m_dataSize.load(std::memory_order_relaxed));
    AUDIO_LOG_INFO(kTag, "跳转位置 -> %u 字节", posBytes);

    p->m_ring.SeekTo(posBytes);   // 只改读位置; 生产者下一轮就把环重置到这里重读
    // 设备内部可能已预取旧位置的数据: 只改游标不冲刷, 会先播一段旧内容再跳
    AAudioStream_requestFlush(p->m_stream);     // 冲刷掉设备缓冲里未播的数据
    return AudioSdk::AudioSdkState::NONE;
}

/**
   @brief : 获取当前播放位置(已喂给设备的字节数, UI 轮询它画进度条)
*/
uint32_t CAudioPlayer::GetPlayPos() const{
    return static_cast<uint32_t>(m_impl->m_ring.Tail());
}

uint32_t CAudioPlayer::GetTotalPos() const{
    return static_cast<uint32_t>(m_impl->m_dataSize.load());
}

/**
   @brief : 当前播放位置(毫秒) —— 字节 ÷ 每秒字节数
   @return : 毫秒; 还没加载文件(字节率为 0)返回 0
*/
uint32_t CAudioPlayer::GetPlayPosMs() const{
    const uint32_t byteRate = m_impl->m_byteRate;
    if (byteRate == 0) return 0;                       // 未加载文件, 别除零
    // 先乘后除(乘 1000ULL 避免 32 位溢出), 拿到的才是毫秒
    const size_t pos = m_impl->m_ring.Tail();
    return static_cast<uint32_t>(pos * 1000ULL / byteRate);
}

/**
   @brief : 总时长(毫秒)
   @return : 毫秒; 还没加载文件(字节率为 0)返回 0
*/
uint32_t CAudioPlayer::GetTotalPosMs() const{
    const uint32_t byteRate = m_impl->m_byteRate;
    if (byteRate == 0) return 0;
    return static_cast<uint32_t>(m_impl->m_dataSize.load() * 1000ULL / byteRate);
}

/**
   @brief : 取整个文件的波形(降采样成 kFilePoints 个峰值点)
   @param : cb - 回调(传 NULL 则什么都不做)
   @param : userData - 透传给回调的指针
   @note 在【调用线程】同步回调一次, 不涉及音频线程。
         分块扫文件算(实现见 CWaveform::BuildFilePeaks), 不是拿整份 PCM ——
         内存不随文件大小涨, 也不会跟生产者抢 m_file(它自己另开一份流)
         回调返回后缓冲即失效, 调用方需要就自己拷走。
*/
void CAudioPlayer::BuildWaveform(AudioSdkWaveCallback cb, void* userData){
    if (!cb) return;

    Impl* p = m_impl;
    if (p->m_dataSize.load(std::memory_order_relaxed) == 0 || p->m_path.empty()) return;   // 还没加载文件

    // 用栈上的定长缓冲: 1024 点 × 2 × 4 字节 = 8KB, 不必分配堆内存
    float minmax[CWaveform::kFilePoints * 2] = {};

    if (p->m_bOgg.load(std::memory_order_relaxed)){
        COggStreamReader r;
        if (r.Open(p->m_path.c_str()) != AudioSdk::AudioSdkState::NONE)
            return;                                        // 打不开, 当没加载
        r.BuildPeaks(minmax, CWaveform::kFilePoints, cb, userData);
        return;
    }

    if (!CWaveform::BuildFilePeaks(p->m_path.c_str(), p->m_payloadOffset,
                                   p->m_dataSize.load(std::memory_order_relaxed),
                                   p->m_encrypted, minmax, CWaveform::kFilePoints))
        return;                                            // 文件打不开, 当没加载

    cb(minmax, CWaveform::kFilePoints, userData);
}

bool CAudioPlayer::IsPlaying() const{
   // 自然播完(回调已返回 STOP)后 m_isPlaying 还是 true、流也还没关,
   // 此时 UI 尚未调 StopPlay, 这里必须如实返回 false, 否则 UI 会一直以为还在播
    return m_impl->m_isPlaying &&
          !m_impl->m_reachedEnd.load(std::memory_order_acquire);
}

bool CAudioPlayer::IsPaused() const{
    return m_impl->m_isPaused;
}

bool CAudioPlayer::GetIsPaused() const{
    return m_impl->m_isPaused;
}

