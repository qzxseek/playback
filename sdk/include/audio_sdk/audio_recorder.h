/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 音频录制接口
*/
#pragma once
#include "audio_sdk/audio_types.h"
#include <string>


class CAudioRecorder{
public:
   CAudioRecorder() = default;
   ~CAudioRecorder() = default;
   // 开始音频录制
   AudioSdk::AudioSdkState StartRecording();
   // 暂停/继续音频录制
   void PauseResumeRecording();
   // 停止音频录制
   AudioSdk::AudioSdkState StopRecording();

   bool SetAencEncrypt();
  
   bool GetAencEncrypt() const;

private:
   // 设备回调函数指针
   static void WaveInProc(HWAVEIN hWaveIn, UINT uMsg, DWORD_PTR dwInstanceData, 
      DWORD_PTR wParam, DWORD_PTR lParam);
   // 缓冲区完成回调函数
   void OnBufferDone(WAVEHDR* hdr);
   
   HWAVEIN   m_hWaveIn   = NULL;         // 句柄
   WAVEHDR   m_waveHdrIn[BUFFER_COUNT];  // 音频缓冲区
   std::vector<BYTE> m_recordedData;     // 录制数据
   bool      m_isRecording = false;      // 是否正在录制
   bool      m_isPaused    = false;      // 是否正在暂停录制
   HWND      m_hWnd      = NULL;         // 窗口句柄
   HWND      m_hBtnPause = NULL;         // 暂停继续按钮动态修改
   bool      m_isAencEncrypt = true;      // 是否加密保存(默认加密)
   std::wstring m_outputName = L"output"; // 输出文件名(不含扩展名, 默认 output)
};

