/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 音频录制实现(Windows, PIMPL: winmm 全部收在 Impl 内, 不泄露到接口头)
*/
#include "audio_sdk/audio_recorder.h"
#include "audio_sdk/common/wav_format.h"   // SaveWavFile：录音落盘统一走它(bEncrypt=true 存加密)

#include <mmeapi.h>
#include <winuser.h>
#include <string>

/**
 * @brief 宽字符路径 → UTF-8(格式层接口统一 UTF-8; 设备层管本地宽路径, 交给格式层前转换)
 * @param wide 宽字符路径
 * @return UTF-8 字符串
 */
static std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0,
                                        nullptr, nullptr);
    if (len <= 1)
        return {};
    std::string utf8(static_cast<size_t>(len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], len, nullptr, nullptr);
    return utf8;
}

/** 
 * @brief 音频录制实现(Windows, PIMPL: winmm 全部收在 Impl 内, 不泄露到接口头)
 * @note 该类负责加载、播放、暂停、停止音频文件。
*/
struct CAudioRecorder::Impl{
   static void WaveInProc(HWAVEIN hWaveIn, UINT uMsg, DWORD_PTR dwInstanceData,
      DWORD_PTR wParam, DWORD_PTR lParam);          // 设备回调函数指针
   void OnBufferDone(WAVEHDR* hdr);                  // 缓冲区完成回调函数

   HWAVEIN   m_hWaveIn   = NULL;         // 句柄
   WAVEHDR   m_waveHdrIn[BUFFER_COUNT];  // 音频缓冲区
   std::vector<BYTE> m_recordedData;     // 录制数据
   bool      m_isRecording = false;      // 是否正在录制
   bool      m_isPaused    = false;      // 是否正在暂停录制
   bool      m_isAencEncrypt = true;     // 是否加密保存(默认加密)
   std::wstring m_outputName = L"output"; // 输出文件名(不含扩展名, 默认 output)
};

/** 
 * @brief 音频录制实现(Windows, PIMPL: winmm 全部收在 Impl 内, 不泄露到接口头)
 * @note 该类负责加载、播放、暂停、停止音频文件。
*/
CAudioRecorder::CAudioRecorder() : m_impl(new Impl()) {}

CAudioRecorder::~CAudioRecorder(){
    if (m_impl){
        StopRecording();          // 若还在录, 先收尾落盘
        delete m_impl;
        m_impl = nullptr;
    }
}

/**
 * @brief 开始音频录制
 * @return 音频录制状态
 */
AudioSdk::AudioSdkState CAudioRecorder::StartRecording(){
   Impl* p = m_impl;

   WAVEFORMATEX fmt = {};        // 清零，避免 cbSize 残留垃圾值
   fmt.wFormatTag = WAVE_FORMAT_PCM;
   fmt.nChannels = CHANNELS;
   fmt.nSamplesPerSec = SAMPLE_RATE;
   fmt.nBlockAlign = CHANNELS * (BITS_PER_SAMPLE / 8);
   fmt.wBitsPerSample = BITS_PER_SAMPLE;
   fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

   MMRESULT res = waveInOpen(&p->m_hWaveIn, WAVE_MAPPER,
      &fmt, (DWORD_PTR)&Impl::WaveInProc,
       (DWORD_PTR)p, CALLBACK_FUNCTION);

   if (res == MMSYSERR_NODRIVER || res == MMSYSERR_BADDEVICEID) {
      return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;        // 无法打开音频设备
   }
   if (res == MMSYSERR_ALLOCATED ) {
      return AudioSdk::AudioSdkState::DEVICE_BUSY;             // 设备已被占用
   }

   p->m_recordedData.clear();     // 清空录制数据
   p->m_isRecording = true;       // 标记为正在录制

   // 初始化缓冲区
   for (int iN = 0; iN < BUFFER_COUNT; iN++){
      ZeroMemory(&p->m_waveHdrIn[iN], sizeof(WAVEHDR));
      p->m_waveHdrIn[iN].lpData = new char[BUFFER_SIZE];       // 分配内存
      p->m_waveHdrIn[iN].dwBufferLength = BUFFER_SIZE;         // 设置缓冲区大小
      waveInPrepareHeader(p->m_hWaveIn, &p->m_waveHdrIn[iN], sizeof(WAVEHDR));
      waveInAddBuffer(p->m_hWaveIn, &p->m_waveHdrIn[iN],sizeof(WAVEHDR));

   }
   waveInStart(p->m_hWaveIn);
   return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 暂停/继续音频录制
 */
void CAudioRecorder::PauseResumeRecording() {
   Impl* p = m_impl;
   // 检查是否正在录制
   if (!p->m_isRecording) return;
   if (p->m_isPaused) {
      waveInStart(p->m_hWaveIn);
      p->m_isPaused = false;
   } else {
      waveInStop(p->m_hWaveIn);        // waveInStop 暂停采集，但不关闭设备
      p->m_isPaused = true;
   }
}

/**
 * @brief 停止音频录制
 * @return 音频录制状态
 */
AudioSdk::AudioSdkState CAudioRecorder::StopRecording() {
   Impl* p = m_impl;
   // 检查是否正在录制
   if (!p->m_isRecording) return AudioSdk::AudioSdkState::NONE;        // 未录制;

   p->m_isRecording = false;
   p->m_isPaused = false;
   // 停止并清空所有缓冲区
   waveInReset(p->m_hWaveIn);
   for (int iN = 0; iN < BUFFER_COUNT; iN++) {
      waveInUnprepareHeader(p->m_hWaveIn, &p->m_waveHdrIn[iN], sizeof(WAVEHDR));
      delete[] p->m_waveHdrIn[iN].lpData;
      p->m_waveHdrIn[iN].lpData = nullptr;
   }
   waveInClose(p->m_hWaveIn);
   p->m_hWaveIn = NULL;

   std::wstring outFile = p->m_outputName;
   outFile += p->m_isAencEncrypt ? L".aenc" : L".wav";
   const std::string utf8Path = WideToUtf8(outFile);   // 宽路径 → UTF-8 再交给格式层
   CWavFormat::SaveWavFile(utf8Path.c_str(), p->m_recordedData.data(),
                           p->m_recordedData.size(), p->m_isAencEncrypt);

   return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 设备回调函数
 */
void CALLBACK CAudioRecorder::Impl::WaveInProc(HWAVEIN hWaveIn, UINT uMsg, DWORD_PTR dwInstanceData,
   DWORD_PTR wParam, DWORD_PTR lParam) {
   if (uMsg == WIM_DATA)     // 系统预定义常量(0x3C4)，录满一个缓冲区时来一次
      reinterpret_cast<Impl*>(dwInstanceData)->OnBufferDone((WAVEHDR*)wParam);
}

/**
 * @brief 缓冲区完成回调函数
 * @param hdr 指向 WAVEHDR 结构体的指针
 */
void CAudioRecorder::Impl::OnBufferDone(WAVEHDR* hdr) {

   // 复制数据到录制数据向量
   m_recordedData.insert(m_recordedData.end(), reinterpret_cast<BYTE*>(hdr->lpData),
   reinterpret_cast<BYTE*> (hdr->lpData) + hdr->dwBytesRecorded);
   if(m_isRecording)
      waveInAddBuffer(m_hWaveIn, hdr, sizeof(WAVEHDR));
}

/**
 * @brief 设置是否加密保存(录制中不生效)
 */
void CAudioRecorder::SetAencEncrypt() {
   Impl* p = m_impl;
   if (p->m_isRecording) return;   // 录制中不允许切换
   p->m_isAencEncrypt = !p->m_isAencEncrypt;
}

bool CAudioRecorder::GetAencEncrypt() const {
   return m_impl->m_isAencEncrypt;
}


bool CAudioRecorder::GetIsPaused() const{
   return m_impl->m_isPaused;
}

size_t CAudioRecorder::GetRecordedBytes() const{
   return m_impl->m_recordedData.size();
}
