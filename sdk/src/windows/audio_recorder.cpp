/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 音频录制实现(Windows, PIMPL: winmm 全部收在 Impl 内, 不泄露到接口头)
*/
#include "audio_sdk/audio_recorder.h"
#include "audio_sdk/wav_format.h"   // SaveWavFile：录音落盘统一走它(bEncrypt=true 存加密)
#include "audio_sdk/waveform.h"     // CWaveform::ComputePeaks(波形降采样)

#include <windows.h>     
#include <mmeapi.h>
#include <winuser.h>
#include <atomic>
#include <string>

// ---- Windows 录音缓冲参数(本文件私有) ----
namespace {
constexpr int kBufferCount = 4;   // 环形缓冲块数: 一块在录, 其余在排队/回调, 避免丢数据
// 每块 100ms
constexpr size_t kBufferSize  = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8) / 10;
}   // namespace

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
   WAVEHDR   m_waveHdrIn[kBufferCount];  // 音频缓冲区
   std::vector<BYTE> m_vecRecData;     // 录制数据
   // 已录字节数
   std::atomic<size_t> m_recordedBytes{0};
   // 跨线程读写: UI 线程置位, 音频线程在 OnBufferDone 里读 → 必须原子
   std::atomic<bool> m_isRecording{false};  // 是否正在录制
   bool      m_isPaused    = false;      // 是否正在暂停录制
   bool      m_isAencEncrypt = true;     // 是否加密保存(默认加密)
   std::wstring m_outputName = L"output"; // 输出文件名(不含扩展名, 默认 output)

   // ---- 波形 ----
   // 回调指针也用原子: UI 线程注册/注销, 音频线程取快照(音频线程不能加锁)
   std::atomic<AudioSdkWaveCallback> m_waveCb{nullptr};
   std::atomic<void*>                m_waveUser{nullptr};
   // 峰值暂存区: 预先分配好 —— 音频线程里只算不分配(分配会导致爆音)
   float m_waveBuf[CWaveform::kPointsPerBlock * 2] = {};
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

   p->m_vecRecData.clear();     // 清空录制数据
   p->m_recordedBytes.store(0, std::memory_order_relaxed);   // 计数跟着清零
   p->m_isRecording = true;       // 标记为正在录制

   // 初始化缓冲区
   for (int iN = 0; iN < kBufferCount; iN++){
      ZeroMemory(&p->m_waveHdrIn[iN], sizeof(WAVEHDR));
      p->m_waveHdrIn[iN].lpData = new char[kBufferSize];       // 分配内存
      p->m_waveHdrIn[iN].dwBufferLength = static_cast<DWORD>(kBufferSize);   // 设置缓冲区大小
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
   for (int iN = 0; iN < kBufferCount; iN++) {
      waveInUnprepareHeader(p->m_hWaveIn, &p->m_waveHdrIn[iN], sizeof(WAVEHDR));
      delete[] p->m_waveHdrIn[iN].lpData;
      p->m_waveHdrIn[iN].lpData = nullptr;
   }
   waveInClose(p->m_hWaveIn);
   p->m_hWaveIn = NULL;

   // 设备已彻底静默(不会再有在途回调), 此时注销波形回调是安全的
   p->m_waveCb.store(nullptr, std::memory_order_release);
   p->m_waveUser.store(nullptr, std::memory_order_relaxed);

   std::wstring outFile = p->m_outputName;
   outFile += p->m_isAencEncrypt ? L".aenc" : L".wav";
   const std::string utf8Path = WideToUtf8(outFile);   // 宽路径 → UTF-8 再交给格式层
   CWavFormat::SaveWavFile(utf8Path.c_str(), p->m_vecRecData.data(),
                           p->m_vecRecData.size(), p->m_isAencEncrypt);

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

   // ---- 波形: 算好这一块的峰值推给调用方 ----
   // 只在"确实还在录"时推: StopRecording 会先把 m_isRecording 置 false,
   // 于是 waveInReset 触发的收尾回调不会再往 UI 推数据(那时 UI 可能正在收尾)。
   const AudioSdkWaveCallback cb = m_waveCb.load(std::memory_order_acquire);
   if (cb && m_isRecording.load(std::memory_order_acquire) &&
       hdr->dwBytesRecorded >= sizeof(int16_t)) {
      // 传字节指针即可, ComputePeaks 内部按字节读(不依赖缓冲的 2 字节对齐)
      CWaveform::ComputePeaks(hdr->lpData, hdr->dwBytesRecorded,
                              m_waveBuf, CWaveform::kPointsPerBlock);
      // 缓冲是 Impl 的成员, 调用方必须在回调返回前拷走(不要保存这个指针)
      cb(m_waveBuf, CWaveform::kPointsPerBlock,
         m_waveUser.load(std::memory_order_relaxed));
   }

   // 复制数据到录制数据向量
   m_vecRecData.insert(m_vecRecData.end(), reinterpret_cast<BYTE*>(hdr->lpData),
   reinterpret_cast<BYTE*> (hdr->lpData) + hdr->dwBytesRecorded);
   m_recordedBytes.store(m_vecRecData.size(), std::memory_order_relaxed);
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

/**
 * @brief 已录制时长(毫秒) —— 已录字节 ÷ 每秒字节数
 * @return 毫秒
 */
uint32_t CAudioRecorder::GetRecordedMs() const{
   const uint32_t byteRate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
   if (byteRate == 0) return 0;
   // 先乘后除(乘 1000ULL 避免 32 位溢出), 拿到的才是毫秒
   return static_cast<uint32_t>(m_impl->m_recordedBytes.load(std::memory_order_relaxed) * 1000ULL / byteRate);
}

/**
 * @brief 注册/取消录音波形回调(录制中每块回调一次)
 * @param cb 回调(传 NULL 取消)
 * @param userData 透传给回调的指针
 * @note 只改两个原子指针不加锁 —— 音频线程用"取快照"的方式读。
 *       先写 userData 再发布 cb(release/acquire 配对), 保证音频线程
 *       看到新回调时一定也能看到配套的 userData。
 */
void CAudioRecorder::SetWaveCallback(AudioSdkWaveCallback cb, void* userData) {
   Impl* p = m_impl;
   p->m_waveUser.store(userData, std::memory_order_relaxed);   // 先给 userData
   p->m_waveCb.store(cb, std::memory_order_release);           // 再发布回调
}
