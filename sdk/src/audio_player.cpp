/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 音频播放实现
*/
#include "audio_sdk/audio_player.h"
#include "audio_sdk/common/wav_validate.h"
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

/**
   @brief : 析构：若还持有设备则先停止，防止泄漏
*/
CAudioPlayer::~CAudioPlayer(){
   Stop();
}

/**
   @brief : 校验头文件、打开设备
   @param : filePath - WAV 文件路径
   @return : 音频设备打开状态
*/
AudioSdk::AudioSdkState CAudioPlayer::PlayWavFile(const wchar_t* filePath){
   // 文件路径出错
   if (!filePath)
      return AudioSdk::AudioSdkState::INVALID_PARAMETER;

   // 读取文件
   std::ifstream file(filePath, std::ios::in | std::ios::binary);
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
   m_vecPcm.assign(vecBuf.begin() + payloadOffset, vecBuf.end());
   if (bEncrypted)
      CEncryptedFormat::XorCrypt(m_vecPcm.data(), m_vecPcm.size());

   // 解析 WAVEFORMATEX
   m_fmt = {};
   m_fmt.wFormatTag = WAVE_FORMAT_PCM;
   m_fmt.nChannels = pHdr->numChannels;
   m_fmt.nSamplesPerSec = pHdr->sampleRate;
   m_fmt.wBitsPerSample = pHdr->bitsPerSample;
   m_fmt.nBlockAlign = pHdr->blockAlign;
   m_fmt.nAvgBytesPerSec = pHdr->byteRate;

   m_dataSize = m_vecPcm.size();  // 记录数据大小
   m_readPos = 0;  // 初始化读取位置为 0
   m_playPos = 0;  // 初始化播放位置为 0

   const size_t blockSize = m_fmt.nBlockAlign * 2048;      // 缓冲区大小 ~46ms/块
   m_vecBlocks.resize(m_iBlockCount);                      
   for (auto& block : m_vecBlocks) block.vecData.resize(blockSize);

   MMRESULT res = waveOutOpen(&m_hWaveOut,WAVE_MAPPED,&m_fmt,
      (DWORD_PTR)&CAudioPlayer::WaveOutProc,(DWORD_PTR)this,CALLBACK_FUNCTION);
   // 部分机器 WAVE_MAPPER 映射损坏（报 BADDEVICEID），此时退回枚举设备逐个试开，
   // 第一个接受该格式的即用
   if (res != MMSYSERR_NOERROR)
   {
      const UINT iDevCount = waveOutGetNumDevs();
      for (UINT id = 0; id < iDevCount && res != MMSYSERR_NOERROR; ++id)
         res = waveOutOpen(&m_hWaveOut,id,&m_fmt,
            (DWORD_PTR)&CAudioPlayer::WaveOutProc,(DWORD_PTR)this,CALLBACK_FUNCTION);
   }
   if (res == WAVERR_BADFORMAT)
      return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;   // 所有设备都不支持该格式
   else if (res == MMSYSERR_ALLOCATED)
      return AudioSdk::AudioSdkState::DEVICE_BUSY;   // 设备已被占用
   else if (res != MMSYSERR_NOERROR)
      return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;   // 设备无法打开

   InitializeCriticalSection(&m_cs);       // 初始化临界区，用于保护缓冲区访问
   m_csInit = true;
   m_hWakeEvent = CreateEvent(NULL, FALSE, FALSE,NULL);
   m_hStopEvent = CreateEvent(NULL, TRUE, FALSE,NULL);

   m_isPlaying = true;
   m_isPaused = false;

   m_hThread = CreateThread(NULL,0,[](LPVOID arg)->DWORD{
      static_cast<CAudioPlayer*>(arg)->FeedLoop();
      return 0;
   },this,0,NULL);
   return AudioSdk::AudioSdkState::NONE;
}

/**
   @brief : 音频播放设备回调函数
   @param : hWaveOut - 音频设备句柄
   @param : uMsg - 消息类型
   @param : dwInstanceData - 实例数据指针
   @param : wParam - 消息参数 1
   @param : lParam - 消息参数 2
*/
void CALLBACK CAudioPlayer::WaveOutProc(HWAVEOUT hWaveOut,
   UINT uMsg, DWORD_PTR dwInstanceData, DWORD_PTR wParam, DWORD_PTR lParam){
   if (uMsg == WOM_DONE){                            // 工作块播放完成
      auto* self = (CAudioPlayer*)dwInstanceData;
      EnterCriticalSection(&self->m_cs);
      WAVEHDR* hdr = (WAVEHDR*)wParam;
      for (auto& block:self->m_vecBlocks)
         if (&block.waveHdr == hdr) {
            block.isDevice = false;                  // 已播放完，标记为闲置
            break;
         }
      LeaveCriticalSection(&self->m_cs);
      SetEvent(self->m_hWakeEvent);  // 唤醒播放线程
   }
}

/**
   @brief : 音频播放线程
*/
void CAudioPlayer::FeedLoop(){
   // 先放入两工作块播放
   for(int iN = 0; iN < m_iBlockCount; iN++) PreparePlay();

   for(;;){
      // 等待唤醒事件或停止事件
      const HANDLE waits[2] = {m_hWakeEvent, m_hStopEvent};
      DWORD dwRet = WaitForMultipleObjects(2, waits, FALSE, 200);
      if (dwRet == WAIT_OBJECT_0 + 1) break;  // 停止播放
      
      EnterCriticalSection(&m_cs);
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
};   

/**
   @brief : 准备下一块数据，播放
*/
bool CAudioPlayer::PreparePlay(){
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
void CAudioPlayer::CleanUpDevice(){
   if (m_hWaveOut) {
      waveOutReset(m_hWaveOut);                     // 缓冲区状态退回   
      for(auto& block : m_vecBlocks) 
         if (block.isDevice) 
         // 取消所有工作块的缓冲区准备
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
AudioSdk::AudioSdkState CAudioPlayer::Seek(DWORD posBytes){
   if (!m_isPlaying) return AudioSdk::AudioSdkState::INVALID_PARAMETER;

   auto align = m_fmt.nBlockAlign;
   if (align) posBytes -= posBytes % align;        // 帧对齐
   if (posBytes >= m_dataSize) posBytes = (DWORD)m_dataSize;

   waveOutPause(m_hWaveOut);          // 暂停播放
   waveOutReset(m_hWaveOut);          // 缓冲区状态退回   

   for(auto& block : m_vecBlocks) 
      if (block.isDevice) 
         // 取消所有工作块的缓冲区准备
         waveOutUnprepareHeader(m_hWaveOut, &block.waveHdr, sizeof(WAVEHDR));
   
   EnterCriticalSection(&m_cs);
   for (auto& block : m_vecBlocks) {
      if (block.waveHdr.dwFlags & WHDR_PREPARED )
         waveOutUnprepareHeader(m_hWaveOut, &block.waveHdr, sizeof(WAVEHDR));
      block.isDevice = false;
   }
   m_readPos = posBytes;
   LeaveCriticalSection(&m_cs);

   SetEvent(m_hWakeEvent);                            // 唤醒播放线程
   if (m_isPaused) waveOutPause(m_hWaveOut);             // 若原先暂停，继续暂停
   return AudioSdk::AudioSdkState::NONE;
}

void CAudioPlayer::Pause(){             // 暂停播放
   if (m_isPaused || !m_isPlaying) return;
   waveOutPause(m_hWaveOut);
   m_isPaused = true;
}
   
void CAudioPlayer::Resume(){             // 继续播放
   if (!m_isPaused || !m_isPlaying) return;
   waveOutRestart(m_hWaveOut);
   m_isPaused = false;
}

/**
   @brief : 停止播放
*/
void CAudioPlayer::Stop(){             // 停止播放
   if (!m_isPlaying) {CleanUpDevice(); return;}
   SetEvent(m_hStopEvent);
   if (m_hThread){                     // 等待线程结束
      WaitForSingleObject(m_hThread, INFINITE);
      CloseHandle(m_hThread);
      m_hThread = NULL;
   }
   CleanUpDevice();
   m_isPaused = false;
   m_isPlaying = false;
}


/**
   @brief : 获取当前播放位置
*/
DWORD CAudioPlayer::GetPlayPos() const{
   return (DWORD)m_playPos;
}

DWORD CAudioPlayer::GetTotalPos() const{
   return (DWORD)m_dataSize;        
}

bool CAudioPlayer::IsPlaying() const{
   return m_isPlaying;
}

bool CAudioPlayer::IsPaused() const{
   return m_isPaused;
}    
bool CAudioPlayer::GetIsPaused() const{
   return m_isPaused;
}
