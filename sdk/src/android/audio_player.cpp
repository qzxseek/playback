/* @Created On : 2026/9/10
   @Author : 孟源
   @note : 音频播放实现(Android, PIMPL: AAudio 全部收在 Impl 内, 不泄露到接口头)
           仅在 Android/NDK 下编译(依赖 <aaudio/AAudio.h>, 要求 API 26+)。
           流式输出: 不为整段 PCM 排队, 而是在 AAudio 回调里按读游标按需喂数据。
           对外接口见 audio_sdk/audio_player.h(平台无关), 与 Windows 侧同名同签名。
*/
#include "audio_sdk/audio_player.h"
#include "audio_sdk/wav_validate.h"
#include "audio_sdk/wav_format.h"
#include "audio_sdk/encrypted_format.h"

#include <aaudio/AAudio.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

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

    void CleanUpStream();             // 停流 + 关流 + 清状态

    std::vector<uint8_t> m_vecPcm;    // 明文 PCM(解密后驻留内存)
    size_t   m_dataSize = 0;          // PCM 总字节数
    size_t   m_readPos  = 0;          // 已喂给设备的字节游标(流式核心)
    int32_t  m_channels = CHANNELS;
    int32_t  m_bytesPerFrame = CHANNELS * (BITS_PER_SAMPLE / 8);

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
    if (!utf8Path)
        return AudioSdk::AudioSdkState::INVALID_PARAMETER;

    // 先清掉上一次(若还在播)
    StopPlay();
    Impl* p = m_impl;

    // 读取整个文件(Android 路径就是 UTF-8, 直接开)
    std::ifstream file(utf8Path, std::ios::in | std::ios::binary);
    if (!file.is_open())
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;

    file.seekg(0, std::ios::end);
    const std::streamsize lFileSize = file.tellg();
    if (lFileSize < 4)
        return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;   // 文件太小, 连魔数都放不下
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> vecBuf(static_cast<size_t>(lFileSize));
    if (!file.read(reinterpret_cast<char*>(vecBuf.data()), lFileSize))
        return AudioSdk::AudioSdkState::FILE_READ_FAILED;

    WavValidate validator;
    const WavHeader* pHdr = nullptr;
    bool bEncrypted  = false;
    size_t payloadOffset = 0;

    if (CEncryptedFormat::IsAencFile(vecBuf.data(), vecBuf.size())){
        if (vecBuf.size() < CEncryptedFormat::kAencPrefixSize + sizeof(WavHeader))
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        // 剥掉 6 字节前缀后校验; 内部 WAV 头自洽(riffSize 等按 44 头算)
        if (!validator.Validate(vecBuf.data() + CEncryptedFormat::kAencPrefixSize,
                                vecBuf.size() - CEncryptedFormat::kAencPrefixSize))
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        pHdr          = &validator.Header();
        bEncrypted    = true;
        payloadOffset = CEncryptedFormat::kAencPrefixSize + sizeof(WavHeader);   // 6+44=50
    }
    else if (vecBuf.size() >= sizeof(WavHeader) &&
                std::memcmp(vecBuf.data(), "RIFF", 4) == 0){
        if (!validator.Validate(vecBuf.data(), vecBuf.size()))
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        pHdr          = &validator.Header();
        payloadOffset = sizeof(WavHeader);
    }
    else
        return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;   // 不是认识的音频格式

    // 数据区: 明文直接取; 加密容器整段 XOR 解回明文(XOR 等长, 长度不变)
    p->m_vecPcm.assign(vecBuf.begin() + payloadOffset, vecBuf.end());
    if (bEncrypted)
        CEncryptedFormat::XorCrypt(p->m_vecPcm.data(), p->m_vecPcm.size());

    p->m_dataSize = p->m_vecPcm.size();
    p->m_readPos  = 0;                                   // 流式游标从头开始
    p->m_channels = pHdr->numChannels ? pHdr->numChannels : CHANNELS;
    p->m_bytesPerFrame = p->m_channels * (pHdr->bitsPerSample / 8);

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK || !builder){
        p->m_vecPcm.clear();
        return AudioSdk::AudioSdkState::PLATFORM_ERROR;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);   // 播放
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);       // 16bit PCM
    AAudioStreamBuilder_setSampleRate(builder, pHdr->sampleRate);        // 用文件头里的采样率
    AAudioStreamBuilder_setChannelCount(builder, p->m_channels);
    AAudioStreamBuilder_setDataCallback(builder, Impl::DataCallback, p); // userData = Impl*
    AAudioStreamBuilder_setErrorCallback(builder, Impl::ErrorCallback, p);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);

    result = AAudioStreamBuilder_openStream(builder, &p->m_stream);
    AAudioStreamBuilder_delete(builder);        // builder 用完立即删, 别漏
    if (result != AAUDIO_OK){
        p->m_vecPcm.clear();
        if (result == AAUDIO_ERROR_INVALID_FORMAT)
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        if (result == AAUDIO_ERROR_UNAVAILABLE)
            return AudioSdk::AudioSdkState::DEVICE_BUSY;
        return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;
    }

    result = AAudioStream_requestStart(p->m_stream);
    if (result != AAUDIO_OK){
        AAudioStream_close(p->m_stream);
        p->m_stream = nullptr;
        p->m_vecPcm.clear();
        return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;
    }

    p->m_isPlaying = true;      // 开流成功后才置位
    p->m_isPaused  = false;
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
   auto* p = reinterpret_cast<Impl*>(userData);
   // 设备被拔 / 出错: 只请求停止, 不在回调线程里 close(由 StopPlay 兜底关闭)
   if (error == AAUDIO_ERROR_DISCONNECTED && p->m_stream)
      AAudioStream_requestStop(stream);
}

/**
   @brief : 流式喂数据(音频线程): 按读游标把下一段 PCM 拷给设备
             设备要多少帧给多少, 喂完为止 —— 这就是"流式", 而非"一次写满"
*/
aaudio_data_callback_result_t CAudioPlayer::Impl::OnAudioReady(
        void* audioData, int32_t numFrames) {
   const int32_t want = numFrames * m_bytesPerFrame;   // 设备本次要的字节数
   auto* out = static_cast<uint8_t*>(audioData);

   if (m_readPos >= m_dataSize){                        // 已喂完
      std::memset(out, 0, static_cast<size_t>(want));   // 尾部补静音
      return AAUDIO_CALLBACK_RESULT_STOP;               // 让 AAudio 自动停流
   }

   const size_t remain = m_dataSize - m_readPos;
   const size_t n = std::min(static_cast<size_t>(want), remain);
   std::memcpy(out, m_vecPcm.data() + m_readPos, n);
   m_readPos += n;                                      // 游标推进

   if (n < static_cast<size_t>(want))
      std::memset(out + n, 0, static_cast<size_t>(want) - n);  // 末尾不足补静音

   return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/**
   @brief : 停流 + 关流 + 清状态(StopPlay 与析构共用)
*/
void CAudioPlayer::Impl::CleanUpStream(){
   m_isPlaying = false;
   m_isPaused  = false;
   if (m_stream){
      AAudioStream_requestStop(m_stream);
      AAudioStream_close(m_stream);
      m_stream = nullptr;
   }
   m_vecPcm.clear();
   m_dataSize = 0;
   m_readPos  = 0;
}

void CAudioPlayer::PausePlay(){             // 暂停播放
   Impl* p = m_impl;
   if (!p->m_isPlaying || p->m_isPaused || !p->m_stream) return;
   // 输出流支持真正的 pause: 保留流位置, Resume 后从原处继续
   AAudioStream_requestPause(p->m_stream);
   p->m_isPaused = true;
}

void CAudioPlayer::ResumePlay(){             // 继续播放
   Impl* p = m_impl;
   if (!p->m_isPlaying || !p->m_isPaused || !p->m_stream) return;
   AAudioStream_requestStart(p->m_stream);   // 读游标 m_readPos 还在原处, 接着喂
   p->m_isPaused = false;
}

/**
   @brief : 停止播放
*/
void CAudioPlayer::StopPlay(){
   m_impl->CleanUpStream();
}

/**
   @brief : 跳转播放位置(改游标 + 冲刷设备已排队数据)
   @param : posBytes - 跳转位置, 单位字节
   @return : 播放状态
*/
AudioSdk::AudioSdkState CAudioPlayer::Seek(uint32_t posBytes){
   Impl* p = m_impl;
   if (!p->m_isPlaying) return AudioSdk::AudioSdkState::INVALID_PARAMETER;

   if (p->m_bytesPerFrame > 0)
      posBytes -= posBytes % static_cast<uint32_t>(p->m_bytesPerFrame);   // 帧对齐
   if (posBytes > p->m_dataSize) posBytes = static_cast<uint32_t>(p->m_dataSize);

   p->m_readPos = posBytes;                    // 改游标
   // 设备内部可能已预取旧位置的数据: 只改游标不冲刷, 会先播一段旧内容再跳
   AAudioStream_requestFlush(p->m_stream);     // 冲刷掉设备缓冲里未播的数据
   return AudioSdk::AudioSdkState::NONE;
}

/**
   @brief : 获取当前播放位置(已喂给设备的字节数, UI 轮询它画进度条)
*/
uint32_t CAudioPlayer::GetPlayPos() const{
   return static_cast<uint32_t>(m_impl->m_readPos);
}

uint32_t CAudioPlayer::GetTotalPos() const{
   return static_cast<uint32_t>(m_impl->m_dataSize);
}

bool CAudioPlayer::IsPlaying() const{
   return m_impl->m_isPlaying;
}

bool CAudioPlayer::IsPaused() const{
   return m_impl->m_isPaused;
}

bool CAudioPlayer::GetIsPaused() const{
   return m_impl->m_isPaused;
}
