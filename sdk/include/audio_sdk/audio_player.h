/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 音频播放接口
*/
#pragma once

#include "audio_sdk/audio_types.h"
#include <vector>

class CAudioPlayer
{
public:
   CAudioPlayer() = default;
   ~CAudioPlayer();

   AudioSdk::AudioSdkState PlayWavFile(const wchar_t* filePath);
   void Pause();              // 暂停播放
   void Resume();             // 恢复播放
   void Stop();               // 停止播放

   AudioSdk::AudioSdkState Seek(DWORD posBytes);        // 设置播放位置
   DWORD GetPlayPos() const;        // 获取当前播放位置
   DWORD GetTotalPos() const;       // 获取总播放时间
   bool IsPlaying() const;        // 是否正在播放
   bool IsPaused() const;           // 是否暂停
   

private:
   static void CALLBACK WaveOutProc(HWAVEOUT hWaveOut, 
      UINT uMsg, DWORD_PTR dwInstanceData, DWORD_PTR wParam, DWORD_PTR lParam);     

   void FeedLoop();                   // 数据加载循环
   bool PreparePlay();                 // 准备播放块与播放
   void CleanUpDevice();                // 清理设备

   static const int m_iBlockCount = 4;      // 缓冲池块数
   static const int m_iDeviceQueue = 2;     // 设备同时拥有块数
   
   HWAVEOUT  m_hWaveOut  = NULL;
   bool      m_isPlaying = false;           // 是否正在播放
   bool      m_isPaused  = false;           // 是否暂停

   std::vector<BYTE> m_vecPcm;              // PCM 数据缓
   size_t m_readPos = 0;                     // 数据加载位置
   size_t m_dataSize = 0;                    // 数据大小
   size_t m_playPos = 0;                     // 播放位置
   WAVEFORMATEX m_fmt = {};                  // 音频格式描述

   struct Block{
      bool isDevice = false;                  // 是否设备块
      std::vector<BYTE> vecData;                // 数据块
      WAVEHDR waveHdr = {};                  // 缓冲区
   };
   std::vector<Block> m_vecBlocks;               // 缓冲池
   CRITICAL_SECTION m_cs;                    // 临界区锁
   bool m_csInit = false;                       // m_cs 是否已 Initialize（垃圾值守护）
   HANDLE m_hThread = NULL;              // 线程句柄
   HANDLE m_hWakeEvent = NULL;                // 唤醒事件句柄   生产者唤醒消费者
   HANDLE m_hStopEvent = NULL;                // 停止事件句柄

};


