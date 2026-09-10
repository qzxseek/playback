/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 音频播放实现(Windows, PIMPL: winmm 全部收在 Impl 内, 不泄露到接口头)
*/
#include "audio_sdk/audio_player.h"
#include "audio_sdk/wav_validate.h"
#include "audio_sdk/wav_format.h"
#include "audio_sdk/encrypted_format.h"

#include <synchapi.h>
#include <windows.h>
#include <mmeapi.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include <cstdio>
#include <string>

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
    bool      m_isPlaying = false;           // 是否正在播放
    bool      m_isPaused  = false;           // 是否暂停

    std::vector<BYTE> m_vecPcm;              // PCM 数据缓
    size_t m_readPos = 0;                     // 数据加载位置
    size_t m_dataSize = 0;                    // 数据大小
    size_t m_playPos = 0;                     // 播放位置
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
};

CAudioPlayer::CAudioPlayer() : m_impl(new Impl()) {}
CAudioPlayer::~CAudioPlayer(){
    if (m_impl){
        m_impl->CleanUpDevice();
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
   if (!utf8Path)
      return AudioSdk::AudioSdkState::INVALID_PARAMETER;

   // 先清掉上一次(若还在播)
   StopPlay();
   Impl* p = m_impl;

   // 读取文件(UTF-8 → 宽, Windows 下宽路径打开不乱码)
   const std::wstring widePath = Utf8ToWide(utf8Path);
   std::ifstream file(widePath, std::ios::in | std::ios::binary);
   if (!file.is_open())
      return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;   // 打开文件失败

   // 整个文件读进内存：播放需要整段 PCM 持续存活到播完，内存缓冲最稳
   file.seekg(0, std::ios::end);
   const std::streamsize lFileSize = file.tellg();
   if (lFileSize < 4){
      return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;   // 文件太小，连魔数都放不下
   }
   file.seekg(0, std::ios::beg);
   std::vector<uint8_t> vecBuf(static_cast<size_t>(lFileSize));
   if (!file.read(reinterpret_cast<char*>(vecBuf.data()), lFileSize))
      return AudioSdk::AudioSdkState::FILE_READ_FAILED;   // 读取失败（文件被截断等）

   WavValidate validator;
   const WavHeader* pHdr = nullptr;
   bool bEncrypted  = false;
   size_t payloadOffset = 0;

   if (CEncryptedFormat::IsAencFile(vecBuf.data(), vecBuf.size())){
      if (vecBuf.size() < CEncryptedFormat::kAencPrefixSize + sizeof(WavHeader))
         return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;   // 头都不完整
      // 剥掉 6 字节前缀后校验；内部 WAV 头自洽（riffSize 等按 44 头算）
      if (!validator.Validate(vecBuf.data() + CEncryptedFormat::kAencPrefixSize,
                              vecBuf.size() - CEncryptedFormat::kAencPrefixSize))
         return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;       // 内部 WAV 头不合法
      pHdr         = &validator.Header();
      bEncrypted   = true;
      payloadOffset = CEncryptedFormat::kAencPrefixSize + sizeof(WavHeader);   // 6+44=50
   }
   else if (vecBuf.size() >= sizeof(WavHeader) &&
            std::memcmp(vecBuf.data(), "RIFF", 4) == 0){
      if (!validator.Validate(vecBuf.data(), vecBuf.size()))
         return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;          // 不是合法的 PCM WAV 文件
      pHdr          = &validator.Header();
      payloadOffset = sizeof(WavHeader);
   }
   else
      return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;             // 不是认识的音频格式

   // 数据区：明文直接取；加密容器整段 XOR 解回明文（XOR 等长，长度不变）
   p->m_vecPcm.assign(vecBuf.begin() + payloadOffset, vecBuf.end());
   if (bEncrypted)
      CEncryptedFormat::XorCrypt(p->m_vecPcm.data(), p->m_vecPcm.size());

   // 解析 WAVEFORMATEX
   p->m_fmt = {};
   p->m_fmt.wFormatTag = WAVE_FORMAT_PCM;
   p->m_fmt.nChannels = pHdr->numChannels;
   p->m_fmt.nSamplesPerSec = pHdr->sampleRate;
   p->m_fmt.wBitsPerSample = pHdr->bitsPerSample;
   p->m_fmt.nBlockAlign = pHdr->blockAlign;
   p->m_fmt.nAvgBytesPerSec = pHdr->byteRate;

   p->m_dataSize = p->m_vecPcm.size();  // 记录数据大小
   p->m_readPos = 0;  // 初始化读取位置为 0
   p->m_playPos = 0;  // 初始化播放位置为 0

   const size_t blockSize = p->m_fmt.nBlockAlign * 2048;      // 缓冲区大小 ~46ms/块
   p->m_vecBlocks.resize(p->m_iBlockCount);
   for (auto& block : p->m_vecBlocks) block.vecData.resize(blockSize);

   MMRESULT res = waveOutOpen(&p->m_hWaveOut,WAVE_MAPPED,&p->m_fmt,
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
   if (res == WAVERR_BADFORMAT)
      return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;   // 所有设备都不支持该格式
   else if (res == MMSYSERR_ALLOCATED)
      return AudioSdk::AudioSdkState::DEVICE_BUSY;   // 设备已被占用
   else if (res != MMSYSERR_NOERROR)
      return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;   // 设备无法打开

   InitializeCriticalSection(&p->m_cs);       // 初始化临界区，用于保护缓冲区访问
   p->m_csInit = true;
   p->m_hWakeEvent = CreateEvent(NULL, FALSE, FALSE,NULL);
   p->m_hStopEvent = CreateEvent(NULL, TRUE, FALSE,NULL);

   p->m_isPlaying = true;
   p->m_isPaused = false;

   p->m_hThread = CreateThread(NULL,0,[](LPVOID arg)->DWORD{
      static_cast<Impl*>(arg)->FeedLoop();
      return 0;
   },p,0,NULL);
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
            block.isDevice = false;                  // 设备已放完, 块回到闲置池
            block.waveHdr.dwFlags = 0;               // 清标志, 供下次 prepare 复用
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

      m_playPos = m_readPos - held;
      bool bDev = false;
      for(auto& block : m_vecBlocks) if (block.isDevice) bDev = true;
      bool bFileDone = (m_readPos >= m_dataSize);
      LeaveCriticalSection(&m_cs);

      // 所有工作块都已播放完，且文件已读取完，播放结束
      if (!bDev && bFileDone) {
         m_isPlaying = false;
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

      size_t szData = min(block.vecData.size(), m_dataSize - m_readPos);             // 尾块可能不足一整块
      block.vecData.assign(m_vecPcm.begin() + m_readPos, m_vecPcm.begin() + m_readPos + szData);   // 只取当前块数据
      m_readPos += szData;

      block.waveHdr = {};                             // 清楚旧数据
      block.waveHdr.lpData = (LPSTR)block.vecData.data();
      block.waveHdr.dwBufferLength = szData;
      block.isDevice = true;

      waveOutPrepareHeader(m_hWaveOut, &block.waveHdr, sizeof(WAVEHDR));  // 打开缓冲区
      waveOutWrite(m_hWaveOut, &block.waveHdr, sizeof(WAVEHDR));
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
   if (!p->m_isPlaying) return AudioSdk::AudioSdkState::INVALID_PARAMETER;

   auto align = p->m_fmt.nBlockAlign;
   if (align) posBytes -= posBytes % align;        // 帧对齐
   if (posBytes >= p->m_dataSize) posBytes = (uint32_t)p->m_dataSize;

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
   LeaveCriticalSection(&p->m_cs);

   SetEvent(p->m_hWakeEvent);                            // 唤醒播放线程, 从新位置补块
   if (p->m_isPaused) waveOutPause(p->m_hWaveOut);       // 若原先暂停，继续暂停
   return AudioSdk::AudioSdkState::NONE;
}

/**
   @brief : 暂停播放
*/
void CAudioPlayer::PausePlay(){             // 暂停播放
   Impl* p = m_impl;
   if (p->m_isPaused || !p->m_isPlaying) return;
   waveOutPause(p->m_hWaveOut);
   p->m_isPaused = true;
}

/**
   @brief : 继续播放
*/
void CAudioPlayer::ResumePlay(){             // 继续播放
   Impl* p = m_impl;
   if (!p->m_isPaused || !p->m_isPlaying) return;
   waveOutRestart(p->m_hWaveOut);
   p->m_isPaused = false;
}

/**
   @brief : 停止播放
*/
void CAudioPlayer::StopPlay(){             // 停止播放
   Impl* p = m_impl;
   if (!p->m_isPlaying) { p->CleanUpDevice(); return; }
   SetEvent(p->m_hStopEvent);
   if (p->m_hThread){                     // 等待线程结束
      WaitForSingleObject(p->m_hThread, INFINITE);
      CloseHandle(p->m_hThread);
      p->m_hThread = NULL;
   }
   p->CleanUpDevice();
   p->m_isPaused = false;
   p->m_isPlaying = false;
}

/**
   @brief : 获取当前播放位置
*/
uint32_t CAudioPlayer::GetPlayPos() const{
   return static_cast<uint32_t>(m_impl->m_playPos);
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
