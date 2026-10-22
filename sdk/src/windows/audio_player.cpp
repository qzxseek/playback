/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 音频播放实现(Windows, PIMPL: winmm 全部收在 Impl 内, 不泄露到接口头)
*/
#include "audio_sdk/audio_player.h"
#include "audio_sdk/wav_validate.h"
#include "audio_sdk/wav_format.h"
#include "audio_sdk/encrypted_format.h"
#include "audio_sdk/ogg_format.h"       
#include "audio_sdk/opus_format.h"     
#include "audio_sdk/waveform.h"
#include "audio_sdk/logging.h"

#include <windows.h>     // 之前靠 audio_types.h 传递进来; 必须排在 synchapi/mmeapi 前面
#include <synchapi.h>
#include <mmeapi.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include <cstdio>
#include <string>

static constexpr const char* kTag = "player";   // 日志来源标记

/**
 * @brief UTF-8 → 宽字符(Windows 路径用宽字符打开才不会中文乱码)
 * @param utf8 UTF-8 字符串指针
 * @return std::wstring 宽字符字符串
*/
static std::wstring Utf8ToWide(const char* utf8){
    if (!utf8)
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 1)
        return {};
    std::wstring wide(static_cast<size_t>(len) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wide[0], len);
    return wide;
}

/** 
 * @brief 音频播放实现(Windows, PIMPL: winmm 全部收在 Impl 内, 不泄露到接口头)
 * @note 该类负责加载、播放、暂停、停止音频文件。
*/
struct CAudioPlayer::Impl
{
    // 设备回调: 只 SetEvent, 不在回调里抢锁(避免与 Seek 死锁)
    static void CALLBACK WaveOutProc(HWAVEOUT hWaveOut, UINT uMsg,
        DWORD_PTR dwInstanceData, DWORD_PTR wParam, DWORD_PTR lParam);

    void FeedLoop();                 // 数据加载循环
    bool PreparePlay();              // 准备播放块与播放
    void CleanUpDevice();            // 清理设备

    static const int m_iBlockCount = 4;      // 缓冲池块数

    HWAVEOUT  m_hWaveOut  = NULL;
  
    std::atomic<bool> m_isPlaying{false};    // 是否正在播放
    bool      m_isPaused  = false;           // 是否暂停(仅 UI 线程碰, 无需原子)

    std::ifstream m_file;
    std::string   m_path;                     // 原路径: BuildWaveform 要另开一份流
    int    m_prepFails = 0;                   // PreparePlay 连续失败次数(成功即清零)
    size_t m_payloadOffset = 0;               // data 块在文件里的起始偏移
    bool   m_encrypted = false;               // 密文容器: 每块要按载荷内偏移解密
    size_t m_readPos = 0;                     // 数据加载位置(载荷内偏移)
    size_t m_dataSize = 0;                    // 数据大小
    std::atomic<size_t> m_playPos{0};         // 播放位置
    WAVEFORMATEX m_fmt = {};                  // 音频格式描述

    struct Block{
        bool isDevice = false;                // 是否设备块
        std::vector<BYTE> vecData;            // 数据块
        WAVEHDR waveHdr = {};                 // 缓冲区
    };
    std::vector<Block> m_vecBlocks;           // 缓冲池
    CRITICAL_SECTION m_cs;                    // 临界区锁
    bool m_csInit = false;                    // m_cs 是否已 Initialize
    HANDLE m_hThread = NULL;                  // 线程句柄
    HANDLE m_hWakeEvent = NULL;               // 唤醒事件句柄   生产者唤醒消费者
    HANDLE m_hStopEvent = NULL;               // 停止事件句柄

    // 压缩流用哪个解码器: 两条链流程一样, 只是读 PCM 的入口不同
    enum class ReaderKind { None, Ogg, Opus };
    COggStreamReader  m_oggReader;
    COpusStreamReader m_opusReader;
    std::atomic<ReaderKind> m_reader{ReaderKind::None};
};

CAudioPlayer::CAudioPlayer() : m_impl(new Impl()) {}
CAudioPlayer::~CAudioPlayer(){
    if (m_impl){
        // 必须走 StopPlay 而不是直接 CleanUpDevice: 播放线程可能还在跑,
        // 只有 StopPlay 会先发停止事件、等线程退出, 再关事件/临界区/设备。
        // 直接清理等于在它脚下抽走 m_cs 和 m_vecBlocks, 线程下一次循环就踩空。
        StopPlay();
        delete m_impl;
        m_impl = nullptr;
    }
}

/**
 * @brief 音频播放设备回调
 * @param hWaveOut 音频设备句柄
 * @param uMsg 消息类型
 * @param dwInstanceData 实现细节指针
 * @param wParam 消息参数1
 * @param lParam 消息参数2
*/
void CALLBACK CAudioPlayer::Impl::WaveOutProc(HWAVEOUT hWaveOut,
   UINT uMsg, DWORD_PTR dwInstanceData, DWORD_PTR wParam, DWORD_PTR lParam){
   if (uMsg == WOM_DONE){                            // 工作块播放完成
      SetEvent(reinterpret_cast<Impl*>(dwInstanceData)->m_hWakeEvent);   // 唤醒播放线程
   }
}

/**
   @brief : 校验头文件、打开设备
   @param : utf8Path - WAV 文件路径(UTF-8)
   @return : 音频设备打开状态
*/
AudioSdk::AudioSdkState CAudioPlayer::PlayWavFile(const char* utf8Path){
   // 文件路径出错
   if (!utf8Path) {
      AUDIO_LOG_ERROR(kTag, "PlayWavFile 路径为 NULL -> INVALID_PARAMETER");
      return AudioSdk::AudioSdkState::INVALID_PARAMETER;
   }

   // 先清掉上一次(若还在播)
   StopPlay();
   Impl* p = m_impl;
   p->m_dataSize = 0;
   p->m_path.clear();

   // 文件常开到播完为止(UTF-8 → 宽, Windows 下宽路径打开不乱码)
   std::ifstream& file = p->m_file;
   file.open(Utf8ToWide(utf8Path), std::ios::in | std::ios::binary);
   if (!file.is_open()) {
      AUDIO_LOG_ERROR(kTag, "打开失败 path=\"%s\" -> FILE_OPEN_FAILED", utf8Path);
      return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;   // 打开文件失败
   }

   file.seekg(0, std::ios::end);
   const std::streamsize lFileSize = file.tellg();
   if (lFileSize < 4){
      AUDIO_LOG_ERROR(kTag, "文件只有 %lld 字节, 魔数都放不下 path=\"%s\" -> FORMAT_NOT_SUPPORTED",
                      static_cast<long long>(lFileSize), utf8Path);
      return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;   // 文件太小，连魔数都放不下
   }

   const size_t prefixSize = lFileSize < 64 * 1024
                           ? static_cast<size_t>(lFileSize) : size_t{64 * 1024};
   file.seekg(0, std::ios::beg);
   std::vector<uint8_t> vecBuf(prefixSize);
   if (!file.read(reinterpret_cast<char*>(vecBuf.data()), static_cast<std::streamsize>(prefixSize))) {
      AUDIO_LOG_ERROR(kTag, "读取失败 path=\"%s\" -> FILE_READ_FAILED", utf8Path);
      return AudioSdk::AudioSdkState::FILE_READ_FAILED;   // 读取失败（文件被截断等）
   }
   const size_t fileSize = static_cast<size_t>(lFileSize);

   WavValidate validator;
   const WavHeader* pHdr = nullptr;
   bool bEncrypted  = false;
   bool bOgg        = false;
   bool bOpus       = false;
   size_t payloadOffset = 0;

   // Opus 也是 OggS 开头, 必须排在 COggFormat 前面, 靠第一页的 OpusHead 区分
   if (COpusFormat::IsAopusData(vecBuf.data(), vecBuf.size())){
      const AudioSdk::AudioSdkState ost = p->m_opusReader.Open(utf8Path);
      if (ost != AudioSdk::AudioSdkState::NONE) {
         AUDIO_LOG_ERROR(kTag, "Aopus 打不开 path=\"%s\" -> 状态码 %d", utf8Path, static_cast<int>(ost));
         return ost;
      }
      bOpus      = true;
      bEncrypted = true;
   }
   else if (COpusFormat::IsOpusData(vecBuf.data(), vecBuf.size())){
      const AudioSdk::AudioSdkState ost = p->m_opusReader.Open(utf8Path);
      if (ost != AudioSdk::AudioSdkState::NONE) {
         AUDIO_LOG_ERROR(kTag, "Opus 打不开 path=\"%s\" -> 状态码 %d", utf8Path, static_cast<int>(ost));
         return ost;
      }
      bOpus = true;
   }
   else if (COggFormat::IsOggData(vecBuf.data(), vecBuf.size())){

      const AudioSdk::AudioSdkState ost = p->m_oggReader.Open(utf8Path);
      if (ost != AudioSdk::AudioSdkState::NONE) {
         AUDIO_LOG_ERROR(kTag, "Ogg 打不开 path=\"%s\" -> 状态码 %d", utf8Path, static_cast<int>(ost));
         return ost;                        // 不是可解的 Vorbis 流
      }
      bOgg = true;
   }
   else if (COggFormat::IsAoggData(vecBuf.data(), vecBuf.size())){
      const AudioSdk::AudioSdkState ost = p->m_oggReader.Open(utf8Path);
      if (ost != AudioSdk::AudioSdkState::NONE) {
         AUDIO_LOG_ERROR(kTag, "Aogg 打不开 path=\"%s\" -> 状态码 %d", utf8Path, static_cast<int>(ost));
         return ost;                        // 不是可解的 Vorbis 流
      }
      bOgg = true;
      bEncrypted = true;
   }
   else if (CEncryptedFormat::IsAencData(vecBuf.data(), vecBuf.size())){
      if (vecBuf.size() < CEncryptedFormat::kAencPrefixSize + sizeof(WavHeader)) {
         AUDIO_LOG_ERROR(kTag, "AENC 容器残缺(%zu 字节, 装不下前缀+头) path=\"%s\" -> FORMAT_NOT_SUPPORTED",
                         vecBuf.size(), utf8Path);
         return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;   // 头都不完整
      }
      // 剥掉 6 字节前缀后校验；内部 WAV 头自洽（riffSize 等按 44 头算）。
      // 总长也要跟着减 6: 头里的 riffSize 描述的是内层 WAV, 不含容器前缀
      if (!validator.Validate(vecBuf.data() + CEncryptedFormat::kAencPrefixSize,
                              vecBuf.size() - CEncryptedFormat::kAencPrefixSize,
                              fileSize - CEncryptedFormat::kAencPrefixSize)) {
         AUDIO_LOG_ERROR(kTag, "AENC 内层 WAV 头不合法 path=\"%s\" -> FORMAT_NOT_SUPPORTED", utf8Path);
         return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;       // 内部 WAV 头不合法
      }
      pHdr         = &validator.Header();
      bEncrypted   = true;
      // 数据偏移由校验器给出: 标准排布是 44, fmt 后夹了 LIST 等子块时是真实位置
      payloadOffset = CEncryptedFormat::kAencPrefixSize + validator.GetDataOffset();
   }
   else if (vecBuf.size() >= sizeof(WavHeader) &&
            std::memcmp(vecBuf.data(), "RIFF", 4) == 0){
      if (!validator.Validate(vecBuf.data(), vecBuf.size(), fileSize)) {
         AUDIO_LOG_ERROR(kTag, "WAV 头校验不过 path=\"%s\" -> FORMAT_NOT_SUPPORTED", utf8Path);
         return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;          // 不是合法的 PCM WAV 文件
      }
      pHdr          = &validator.Header();
      payloadOffset = validator.GetDataOffset();
   }
   else {
      AUDIO_LOG_ERROR(kTag, "既非 Ogg 也非 AENC 也非 RIFF path=\"%s\" -> FORMAT_NOT_SUPPORTED", utf8Path);
      return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;             // 不是认识的音频格式
   }

   p->m_path = utf8Path;
   p->m_reader.store(bOpus ? Impl::ReaderKind::Opus
                   : bOgg  ? Impl::ReaderKind::Ogg
                           : Impl::ReaderKind::None,
                    std::memory_order_relaxed);
   p->m_fmt  = {};
   p->m_fmt.wFormatTag = WAVE_FORMAT_PCM;

   if (bOgg || bOpus){
      // 两条解码链的接口一一对应, 取值时二选一
      const int32_t iChannels = bOpus ? p->m_opusReader.GetChannels()      : p->m_oggReader.GetChannels();
      const int32_t iRate     = bOpus ? p->m_opusReader.GetSampleRate()    : p->m_oggReader.GetSampleRate();
      const uint16_t wBits    = bOpus ? p->m_opusReader.GetBitsPerSample() : p->m_oggReader.GetBitsPerSample();
      const uint16_t wAlign   = bOpus ? p->m_opusReader.GetBlockAlign()    : p->m_oggReader.GetBlockAlign();
      const size_t nTotal     = bOpus ? p->m_opusReader.GetTotalBytes()    : p->m_oggReader.GetTotalBytes();

      p->m_fmt.nChannels      = static_cast<WORD>(iChannels);
      p->m_fmt.nSamplesPerSec = static_cast<DWORD>(iRate);
      p->m_fmt.wBitsPerSample = wBits;
      p->m_fmt.nBlockAlign    = wAlign;
      // 解码器只吐 16bit PCM, 每秒字节数按定义算就行
      p->m_fmt.nAvgBytesPerSec = p->m_fmt.nSamplesPerSec * p->m_fmt.nBlockAlign;
      p->m_encrypted     = false;           // 解密在解码器内部做, 这里再解一遍就是解回密文
      p->m_payloadOffset = 0;               // 压缩流用不上文件偏移, 解码器自己记位置
      p->m_dataSize      = nTotal;
   }
   else {
      p->m_payloadOffset = payloadOffset;
      p->m_encrypted     = bEncrypted;
      p->m_dataSize      = validator.GetDataSize();

      // 解析 WAVEFORMATEX
      p->m_fmt.nChannels = pHdr->numChannels;
      p->m_fmt.nSamplesPerSec = pHdr->sampleRate;
      p->m_fmt.wBitsPerSample = pHdr->bitsPerSample;
      p->m_fmt.nBlockAlign = pHdr->blockAlign;
      p->m_fmt.nAvgBytesPerSec = pHdr->byteRate;
   }

   p->m_readPos   = 0;  // 初始化读取位置为 0
   p->m_prepFails = 0;  // 上一轮的失败计数不带进这一轮
   p->m_playPos.store(0, std::memory_order_relaxed);  // 初始化播放位置为 0

   const size_t blockSize = p->m_fmt.nBlockAlign * 2048;      // 缓冲区大小 ~46ms/块
   p->m_vecBlocks.resize(p->m_iBlockCount);
   for (auto& block : p->m_vecBlocks) block.vecData.resize(blockSize);

   MMRESULT res = waveOutOpen(&p->m_hWaveOut,WAVE_MAPPER,&p->m_fmt,
      (DWORD_PTR)&Impl::WaveOutProc,(DWORD_PTR)p,CALLBACK_FUNCTION);
   // 部分机器 WAVE_MAPPER 映射损坏（报 BADDEVICEID），此时退回枚举设备逐个试开，
   // 第一个接受该格式的即用
   if (res != MMSYSERR_NOERROR)
   {
      const UINT iDevCount = waveOutGetNumDevs();
      for (UINT id = 0; id < iDevCount && res != MMSYSERR_NOERROR; ++id)
         res = waveOutOpen(&p->m_hWaveOut,id,&p->m_fmt,
            (DWORD_PTR)&Impl::WaveOutProc,(DWORD_PTR)p,CALLBACK_FUNCTION);
   }
   if (res == WAVERR_BADFORMAT){
      AUDIO_LOG_ERROR(kTag, "waveOutOpen 无设备支持该格式 %uch %uHz %ubit (MMRESULT=%u) -> FORMAT_NOT_SUPPORTED",
                      p->m_fmt.nChannels, p->m_fmt.nSamplesPerSec, p->m_fmt.wBitsPerSample,
                      static_cast<unsigned>(res));
      p->m_dataSize = 0;
      p->m_fmt = {};
      return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;   // 所有设备都不支持该格式
   }
   else if (res == MMSYSERR_ALLOCATED){
      AUDIO_LOG_ERROR(kTag, "waveOutOpen 设备已被占用 (MMRESULT=%u) -> DEVICE_BUSY",
                      static_cast<unsigned>(res));
      p->m_dataSize = 0;
      p->m_fmt = {};
      return AudioSdk::AudioSdkState::DEVICE_BUSY;   // 设备已被占用
   }
   else if (res != MMSYSERR_NOERROR){
      AUDIO_LOG_ERROR(kTag, "waveOutOpen 失败, 枚举 %u 个设备都没开起来 (MMRESULT=%u) -> DEVICE_NOT_FOUND",
                      static_cast<unsigned>(waveOutGetNumDevs()), static_cast<unsigned>(res));
      p->m_dataSize = 0;
      p->m_fmt = {};
      return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;   // 设备无法打开
   }
   InitializeCriticalSection(&p->m_cs);       // 初始化临界区，用于保护缓冲区访问
   p->m_csInit = true;
   p->m_hWakeEvent = CreateEvent(NULL, FALSE, FALSE,NULL);
   p->m_hStopEvent = CreateEvent(NULL, TRUE, FALSE,NULL);
   // 事件建失败(极少, 一般是句柄耗尽)就整体回滚, 不能带着空句柄往下走:
   if (p->m_hWakeEvent == NULL || p->m_hStopEvent == NULL) {
      AUDIO_LOG_ERROR(kTag, "CreateEvent 失败 (Win32=%lu, 多半是句柄耗尽) -> UNKNOWN_ERROR",
                      ::GetLastError());
      p->CleanUpDevice();
      return AudioSdk::AudioSdkState::UNKNOWN_ERROR;
   }

   p->m_isPlaying.store(true, std::memory_order_relaxed);
   p->m_isPaused = false;

   p->m_hThread = CreateThread(NULL,0,[](LPVOID arg)->DWORD{
      static_cast<Impl*>(arg)->FeedLoop();
      return 0;
   },p,0,NULL);
   if (p->m_hThread == NULL) {                // 线程没起来, 状态和设备都回滚
      AUDIO_LOG_ERROR(kTag, "CreateThread 失败 (Win32=%lu) -> UNKNOWN_ERROR", ::GetLastError());
      p->m_isPlaying.store(false, std::memory_order_relaxed);
      p->CleanUpDevice();
      return AudioSdk::AudioSdkState::UNKNOWN_ERROR;
   }

   // 成功: 记"播的是哪个文件 + 什么格式"。要回答的是"当时放的是不是我以为的那个" ——
   // 路径给这个答案, 格式和时长给"内容有没有被截断/是不是变样了"
   const uint32_t byteRate = p->m_fmt.nAvgBytesPerSec;
   const char* const container = bOpus ? (bEncrypted ? "aopus" : "opus")
                               : bOgg  ? (bEncrypted ? "aogg"  : "ogg")
                                       : (bEncrypted ? "aenc"  : "wav");
   AUDIO_LOG_INFO(kTag, "开始播放 path=\"%s\" %s %uch %uHz %ubit %.2fs (%zu 字节 PCM)",
                  utf8Path, container,
                  p->m_fmt.nChannels, p->m_fmt.nSamplesPerSec, p->m_fmt.wBitsPerSample,
                  byteRate ? static_cast<double>(p->m_dataSize) / byteRate : 0.0,
                  p->m_dataSize);
   return AudioSdk::AudioSdkState::NONE;
}

/**
   @brief : 音频播放线程
*/
void CAudioPlayer::Impl::FeedLoop(){

   EnterCriticalSection(&m_cs);
   for(int iN = 0; iN < m_iBlockCount; iN++) PreparePlay();
   LeaveCriticalSection(&m_cs);

   for(;;){
      // 等待唤醒事件或停止事件
      const HANDLE waits[2] = {m_hWakeEvent, m_hStopEvent};
      DWORD dwRet = WaitForMultipleObjects(2, waits, FALSE, 200);
      if (dwRet == WAIT_OBJECT_0 + 1) break;  // 停止播放

      EnterCriticalSection(&m_cs);

      // 1) 回收已播完的块: 回调不再改状态(防死锁), 由本线程查 WHDR_DONE 发现
      for (auto& block : m_vecBlocks){
         if (block.isDevice && (block.waveHdr.dwFlags & WHDR_DONE)){
            waveOutUnprepareHeader(m_hWaveOut, &block.waveHdr, sizeof(WAVEHDR));
            block.waveHdr.dwFlags = 0;               // 清标志, 供下次 prepare 复用
            block.isDevice = false;                  // 设备已放完, 块回到闲置池
         }
      }

      // 2) 补块: 闲置块少于阈值就再喂给设备
      int iDev = 0;        // 获取当前多少工作块
      for(auto& block : m_vecBlocks) if (block.isDevice) iDev++;
      while(iDev < m_iBlockCount && m_readPos < m_dataSize) {
         if (PreparePlay()) iDev++;
         else break;
      }

      // 更新进度：已喂给设备 - 设备未播完的部分 = 已播放位置
      DWORD held = 0;                  // 未播放完的部分
      for(auto& block : m_vecBlocks) if (block.isDevice) held += block.waveHdr.dwBufferLength;

      m_playPos.store(m_readPos - held, std::memory_order_relaxed);
      bool bDev = false;
      for(auto& block : m_vecBlocks) if (block.isDevice) bDev = true;
      bool bFileDone = (m_readPos >= m_dataSize);
      LeaveCriticalSection(&m_cs);

      // 所有工作块都已播放完，且文件已读取完，播放结束
      if (!bDev && bFileDone) {
         m_isPlaying.store(false, std::memory_order_relaxed);
         break;
      }
   }
}

/**
   @brief : 准备下一块数据，播放
*/
bool CAudioPlayer::Impl::PreparePlay(){
   for(auto& block : m_vecBlocks){
      if (block.isDevice) continue;          // 寻找闲置数据块
      if(m_readPos >= m_dataSize) return false;  // 已读取完数据，返回

      size_t szData = min(block.vecData.size(), m_dataSize - m_readPos);   // 尾块可能不足一整块

      const ReaderKind eKind = m_reader.load(std::memory_order_relaxed);
      if (eKind != ReaderKind::None){
         szData = (eKind == ReaderKind::Opus)
                      ? m_opusReader.Read(block.vecData.data(), szData)
                      : m_oggReader.Read(block.vecData.data(), szData);
         if (szData == 0) {           // 读干净了(或解码出错): 当作文件到此为止
            m_readPos = m_dataSize;
            return false;
         }
      }
      else {
         // 现读现解: 定位到这块在文件里的位置, 直接读进设备缓冲
         m_file.seekg(static_cast<std::streamoff>(m_payloadOffset + m_readPos));
         if (!m_file.read(reinterpret_cast<char*>(block.vecData.data()),
                          static_cast<std::streamsize>(szData))) {
            // 读不出来(文件被截断/被别的程序截走)就当文件到此为止: 游标推到末尾
            m_readPos = m_dataSize;
            return false;
         }
         // 解密
         if (m_encrypted)
            CEncryptedFormat::XorCrypt(block.vecData.data(), szData, m_readPos);
      }

      block.waveHdr = {};                             // 清楚旧数据
      block.waveHdr.lpData = (LPSTR)block.vecData.data();
      block.waveHdr.dwBufferLength = szData;

      MMRESULT res = waveOutPrepareHeader(m_hWaveOut, &block.waveHdr, sizeof(WAVEHDR));
      if (res == MMSYSERR_NOERROR)
         res = waveOutWrite(m_hWaveOut, &block.waveHdr, sizeof(WAVEHDR));
      if (res != MMSYSERR_NOERROR){
         // 回滚: 块必须留在闲置池里。否则 isDevice 一直是 true、却永远等不到
         if (block.waveHdr.dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(m_hWaveOut, &block.waveHdr, sizeof(WAVEHDR));
         block.waveHdr = {};
         block.isDevice = false;

         // 压缩流的解码器已经吃过这一段了, 得回到原位置才能重读
         if (eKind == ReaderKind::Opus)
            m_opusReader.SeekTo(m_readPos);
         else if (eKind == ReaderKind::Ogg)
            m_oggReader.SeekTo(m_readPos);

         // 连着失败多半是设备没了 —— 再重试也是同一个结果
         if (++m_prepFails >= 8){
            AUDIO_LOG_ERROR(kTag, "连续 %d 次写入设备失败(MMRESULT=%u), 停止播放",
                            m_prepFails, static_cast<unsigned>(res));
            m_readPos = m_dataSize;
         }
         return false;
      }

      block.isDevice = true;
      m_readPos += szData;
      m_prepFails = 0;
      return true;
   }
   return false;
}

/**
   @brief : 清理设备设备资源
*/
void CAudioPlayer::Impl::CleanUpDevice(){
   if (m_hWaveOut) {
      waveOutReset(m_hWaveOut);                     // 缓冲区状态退回
      for(auto& block : m_vecBlocks)
         if (block.waveHdr.dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(m_hWaveOut, &block.waveHdr, sizeof(WAVEHDR));
      waveOutClose(m_hWaveOut);
      m_hWaveOut = NULL;
   }
   if (m_hWakeEvent) {
      CloseHandle(m_hWakeEvent);
      m_hWakeEvent = NULL;
   }
   if (m_hStopEvent) {
      CloseHandle(m_hStopEvent);
      m_hStopEvent = NULL;
   }
   if (m_file.is_open())
      m_file.close();
   m_oggReader.Close();
   m_opusReader.Close();
   m_reader.store(ReaderKind::None, std::memory_order_relaxed);
   if (m_csInit){
      DeleteCriticalSection(&m_cs);
      m_csInit = false;
   }
}

/**
   @brief : 跳转播放位置
   @param : posBytes - 跳转位置，单位字节
   @return 音频播放状态
*/
AudioSdk::AudioSdkState CAudioPlayer::Seek(uint32_t posBytes){
   Impl* p = m_impl;
   if (!p->m_isPlaying.load(std::memory_order_relaxed)) {
      AUDIO_LOG_WARN(kTag, "Seek(%u) 但没在播 -> INVALID_PARAMETER", posBytes);
      return AudioSdk::AudioSdkState::INVALID_PARAMETER;
   }

   auto align = p->m_fmt.nBlockAlign;
   if (align) posBytes -= posBytes % align;        // 帧对齐
   if (posBytes >= p->m_dataSize) posBytes = (uint32_t)p->m_dataSize;
   AUDIO_LOG_INFO(kTag, "跳转位置 -> %u 字节 (%u ms)", posBytes, GetPlayPosMs());

   waveOutPause(p->m_hWaveOut);          // 暂停播放
   EnterCriticalSection(&p->m_cs);
   waveOutReset(p->m_hWaveOut);          // 清掉"等锁期间播放线程新写的块"

   for (auto& block : p->m_vecBlocks){
      if (block.waveHdr.dwFlags & WHDR_PREPARED)
         waveOutUnprepareHeader(p->m_hWaveOut, &block.waveHdr, sizeof(WAVEHDR));
   }
   for (auto& block : p->m_vecBlocks) {
      block.isDevice = false;              // 全部标记闲置
      block.waveHdr.dwFlags = 0;           // 清 DONE/PREPARED 位, 供下次复用
   }
   p->m_readPos = posBytes;
   // 压缩流必须让解码器一起跳
   const Impl::ReaderKind eKind = p->m_reader.load(std::memory_order_relaxed);
   if (eKind == Impl::ReaderKind::Opus)
      p->m_opusReader.SeekTo(posBytes);
   else if (eKind == Impl::ReaderKind::Ogg)
      p->m_oggReader.SeekTo(posBytes);
   LeaveCriticalSection(&p->m_cs);

   SetEvent(p->m_hWakeEvent);                            // 唤醒播放线程, 从新位置补块
   if (p->m_isPaused) waveOutPause(p->m_hWaveOut);       // 若原先暂停，继续暂停
   else waveOutRestart(p->m_hWaveOut);                  // 若原先在播放，继续播放
   return AudioSdk::AudioSdkState::NONE;
}

/**
   @brief : 暂停播放
*/
void CAudioPlayer::PausePlay(){             // 暂停播放
   Impl* p = m_impl;
   if (p->m_isPaused || !p->m_isPlaying.load(std::memory_order_relaxed)) return;
   waveOutPause(p->m_hWaveOut);
   p->m_isPaused = true;
   AUDIO_LOG_INFO(kTag, "暂停于 %u ms", GetPlayPosMs());
}

/**
   @brief : 继续播放
*/
void CAudioPlayer::ResumePlay(){             // 继续播放
   Impl* p = m_impl;
   if (!p->m_isPaused || !p->m_isPlaying.load(std::memory_order_relaxed)) return;
   waveOutRestart(p->m_hWaveOut);
   p->m_isPaused = false;
   AUDIO_LOG_INFO(kTag, "继续于 %u ms", GetPlayPosMs());
}

/**
   @brief : 停止播放
*/
void CAudioPlayer::StopPlay(){             // 停止播放
   Impl* p = m_impl;
   if (!p->m_isPlaying.load(std::memory_order_relaxed)) {
      // 句柄存在, 等待线程退出后置空
      if (p->m_hThread) {
         WaitForSingleObject(p->m_hThread, INFINITE);   // 等它彻底退出再关
         CloseHandle(p->m_hThread);
         p->m_hThread = NULL;
      }
      p->CleanUpDevice();
      return;
   }
   
   AUDIO_LOG_INFO(kTag, "停止于 %u ms / 共 %u ms", GetPlayPosMs(), GetTotalPosMs());

   if (p->m_hStopEvent)
      SetEvent(p->m_hStopEvent);

   if (p->m_hThread){                     // 等待线程结束
      WaitForSingleObject(p->m_hThread, INFINITE);
      CloseHandle(p->m_hThread);
      p->m_hThread = NULL;
   }
   p->CleanUpDevice();
   p->m_isPaused = false;
   p->m_isPlaying.store(false, std::memory_order_relaxed);
}

/**
   @brief : 获取当前播放位置
*/
uint32_t CAudioPlayer::GetPlayPos() const{
   return static_cast<uint32_t>(m_impl->m_playPos.load(std::memory_order_relaxed));
}

uint32_t CAudioPlayer::GetTotalPos() const{
   return static_cast<uint32_t>(m_impl->m_dataSize);
}

/**
 * @brief 当前播放位置(毫秒) —— 字节 ÷ 每秒字节数
 * @return 毫秒; 还没加载文件(字节率为 0)返回 0
 */
uint32_t CAudioPlayer::GetPlayPosMs() const{
   const uint32_t byteRate = m_impl->m_fmt.nAvgBytesPerSec;
   if (byteRate == 0) return 0;                       // 未加载文件, 别除零
   // 先乘后除(乘 1000ULL 避免 32 位溢出), 拿到的才是毫秒
   return static_cast<uint32_t>(m_impl->m_playPos.load(std::memory_order_relaxed) * 1000ULL / byteRate);
}

/**
 * @brief 总时长(毫秒)
 * @return 毫秒; 还没加载文件(字节率为 0)返回 0
 */
uint32_t CAudioPlayer::GetTotalPosMs() const{
   const uint32_t byteRate = m_impl->m_fmt.nAvgBytesPerSec;
   if (byteRate == 0) return 0;
   return static_cast<uint32_t>(m_impl->m_dataSize * 1000ULL / byteRate);
}

/**
 * @brief 取整个文件的波形(降采样成 kFilePoints 个峰值点)
 * @param cb 回调(传 NULL 则什么都不做)
 * @param userData 透传给回调的指针
 * @note 在调用线程同步跑完, 不涉及音频线程(回调也在本线程里, 顺序发生)
 *       分块算, 而不是拿整份 PCM —— 内存不随文件大小涨
 *       回调【可能来多次】, 每次的 points 是"已算出几个点"(WAV 那条路一次给全;
 *       Ogg 是解一段报一段)。回调返回后缓冲即失效, 调用方需要就自己拷走
 */
void CAudioPlayer::BuildWaveform(AudioSdkWaveCallback cb, void* userData){
   if (!cb) return;

   Impl* p = m_impl;
   if (p->m_dataSize == 0 || p->m_path.empty()) return;   // 还没加载文件

   // 用栈上的定长缓冲: 1024 点 × 2 × 4 字节 = 8KB, 栈上放得下, 不必分配堆内存
   float minmax[CWaveform::kFilePoints * 2] = {};

   const Impl::ReaderKind eKind = p->m_reader.load(std::memory_order_relaxed);
   if (eKind == Impl::ReaderKind::Opus){
      COpusStreamReader r;
      if (r.Open(p->m_path.c_str()) != AudioSdk::AudioSdkState::NONE)
         return;                                          // 打不开, 当没加载
      r.BuildPeaks(minmax, CWaveform::kFilePoints, cb, userData);
      return;
   }
   if (eKind == Impl::ReaderKind::Ogg){
      COggStreamReader r;
      if (r.Open(p->m_path.c_str()) != AudioSdk::AudioSdkState::NONE)
         return;                                          // 打不开, 当没加载
      r.BuildPeaks(minmax, CWaveform::kFilePoints, cb, userData);
      return;
   }

   // 分块扫文件算, 内存固定
   if (!CWaveform::BuildFilePeaks(p->m_path.c_str(), p->m_payloadOffset, p->m_dataSize,
                                 p->m_encrypted, minmax, CWaveform::kFilePoints))
      return;                                             // 文件打不开, 当没加载

   cb(minmax, CWaveform::kFilePoints, userData);
}

bool CAudioPlayer::IsPlaying() const{
   return m_impl->m_isPlaying.load(std::memory_order_relaxed);
}

bool CAudioPlayer::IsPaused() const{
   return m_impl->m_isPaused;
}
bool CAudioPlayer::GetIsPaused() const{
   return m_impl->m_isPaused;
}

