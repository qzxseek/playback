/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 音频录制实现
*/
#include "audio_sdk/audio_recorder.h"
#include "audio_sdk/wav_format.h"   // SaveWavFile：录音落盘统一走它(bEncrypt=true 存加密)

#include <mmeapi.h>
#include <winuser.h>

/** 
 * @brief 开始音频录制
 * @return 音频录制状态
 */
AudioSdk::AudioSdkState CAudioRecorder::StartRecording(){

   WAVEFORMATEX fmt = {};        // 清零，避免 cbSize 残留垃圾值
   fmt.wFormatTag = WAVE_FORMAT_PCM;
   fmt.nChannels = CHANNELS;
   fmt.nSamplesPerSec = SAMPLE_RATE;
   fmt.nBlockAlign = CHANNELS * (BITS_PER_SAMPLE / 8);
   fmt.wBitsPerSample = BITS_PER_SAMPLE;
   fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

   
   // CALLBACK_WINDOW：录满一个缓冲区时向窗口发送 WM_WAVEIN_DONE 消息，
   // 需由窗口过程收集数据并重新挂回缓冲区（见 audio_types.h 中 WM_WAVEIN_DONE）
   MMRESULT res = waveInOpen(&m_hWaveIn, WAVE_MAPPER, 
      &fmt, (DWORD_PTR)&CAudioRecorder::WaveInProc,
       (DWORD_PTR)this, CALLBACK_FUNCTION);
      
   if (res == MMSYSERR_NODRIVER || res == MMSYSERR_BADDEVICEID) {
      return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;        // 无法打开音频设备
   }
   if (res == MMSYSERR_ALLOCATED ) {
      return AudioSdk::AudioSdkState::DEVICE_BUSY;             // 设备已被占用
   }

   m_recordedData.clear();     // 清空录制数据
   m_isRecording = true;       // 标记为正在录制

   // 初始化缓冲区
   for (int iN = 0; iN < BUFFER_COUNT; iN++){
      ZeroMemory(&m_waveHdrIn[iN], sizeof(WAVEHDR));
      m_waveHdrIn[iN].lpData = new char[BUFFER_SIZE];       // 分配内存
      m_waveHdrIn[iN].dwBufferLength = BUFFER_SIZE;         // 设置缓冲区大小
      waveInPrepareHeader(m_hWaveIn, &m_waveHdrIn[iN], sizeof(WAVEHDR));
      waveInAddBuffer(m_hWaveIn, &m_waveHdrIn[iN],sizeof(WAVEHDR));
      
   }

   waveInStart(m_hWaveIn);
   SetWindowTextW(m_hBtnPause, L"⏸ 暂停");
   EnableWindow(m_hBtnPause, TRUE);
   return AudioSdk::AudioSdkState::NONE;
}
/** 
 * @brief 暂停/继续音频录制
 */
void CAudioRecorder::PauseResumeRecording() {
   // 检查是否正在录制
   if (!m_isRecording) return;
   if (m_isPaused) {
      waveInStart(m_hWaveIn);
      SetWindowTextW(m_hBtnPause, L"⏸ 暂停");
      m_isPaused = false;
   } else {
      waveInStop(m_hWaveIn);        // waveInStop 暂停采集，但不关闭设备
      SetWindowTextW(m_hBtnPause, L"▶ 继续");
      m_isPaused = true;
   }
}

/** 
 * @brief 停止音频录制
 * @return 音频录制状态
 */
AudioSdk::AudioSdkState CAudioRecorder::StopRecording() {
   // 检查是否正在录制
   if (!m_isRecording) return AudioSdk::AudioSdkState::NONE;        // 未录制;

   m_isRecording = false;
   m_isPaused = false;
   // 停止并清空所有缓冲区
   waveInReset(m_hWaveIn); 
   for (int iN = 0; iN < BUFFER_COUNT; iN++) {
      waveInUnprepareHeader(m_hWaveIn, &m_waveHdrIn[iN], sizeof(WAVEHDR));
      delete[] m_waveHdrIn[iN].lpData;
      m_waveHdrIn[iN].lpData = nullptr;
   }
   waveInClose(m_hWaveIn);
   m_hWaveIn = NULL;

   // 保存录音数据：统一走 SaveWavFile, bEncrypt=true 落盘加密容器 .aenc
   EnableWindow(m_hBtnPause, FALSE);
   CWavFormat::SaveWavFile(m_outputFile, m_recordedData.data(),
                           m_recordedData.size(), m_isAencEncrypt);

   return AudioSdk::AudioSdkState::NONE;
}
/** 
 * @brief 设备回调函数
 * @param hWaveIn 音频设备句柄
 * @param msg 消息类型
 * @param dwInstanceData 实例数据指针
 * @param wParam 消息参数1
 * @param lParam 消息参数2，指向 WAVEHDR 结构体的指针
*/
void CALLBACK CAudioRecorder::WaveInProc(HWAVEIN hWaveIn, UINT uMsg, DWORD_PTR dwInstanceData, 
   DWORD_PTR wParam, DWORD_PTR lParam) {
   if (uMsg == WIM_DATA)     // 系统预定义常量(0x3C4)，录满一个缓冲区时来一次
      reinterpret_cast<CAudioRecorder*>(dwInstanceData)->OnBufferDone((WAVEHDR*)wParam);
}
/** 
 * @brief 缓冲区完成回调函数
 * @param hdr 指向 WAVEHDR 结构体的指针
 */
void CAudioRecorder::OnBufferDone(WAVEHDR* hdr) {

   // 复制数据到录制数据向量
   m_recordedData.insert(m_recordedData.end(), reinterpret_cast<BYTE*>(hdr->lpData),
   reinterpret_cast<BYTE*> (hdr->lpData) + hdr->dwBytesRecorded);
   if(m_isRecording)
      waveInAddBuffer(m_hWaveIn, hdr, sizeof(WAVEHDR));
}

void CAudioRecorder::SetAencEncrypt(bool& bEncrypt) {
   if (m_isRecording) bEncrypt = false;
   else bEncrypt = true;
}

// 获取是否加密保存
bool CAudioRecorder::GetAencEncrypt() const {
   return m_isAencEncrypt;
}
