/* @Created On : 2026/8/10
   @Author : 孟源
   @note : 音频录制实现(Windows, PIMPL: winmm 全部收在 Impl 内, 不泄露到接口头)
*/
#include "audio_sdk/audio_recorder.h"
#include "audio_sdk/wav_format.h"
#include "audio_sdk/ogg_format.h"
#include "audio_sdk/opus_format.h"  
#include "audio_sdk/waveform.h"     // CWaveform::ComputePeaks(波形降采样)
#include "audio_sdk/wave_ring.h"    // CWaveRing: 音频线程写、调用线程读的峰值环形缓冲
#include "audio_sdk/logging.h"

#include <windows.h>
#include <mmeapi.h>
#include <winuser.h>
#include <atomic>
#include <new>
#include <string>

static constexpr const char* kTag = "recorder";   // 日志来源标记

// ---- Windows 录音缓冲参数(本文件私有) ----
namespace {
constexpr int kBufferCount = 4;   // 环形缓冲块数: 一块在录, 其余在排队/回调, 避免丢数据
// 每块 AUDIO_SDK_BLOCK_MS 毫秒的字节数(先乘后除: 这里的取值都能整除, 无截断)
constexpr size_t kBufferSize =
   SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8) * AUDIO_SDK_BLOCK_MS / 1000;
// 块长必须是整帧的倍数, 否则一个 16bit 采样会被切在相邻两块里 —— 录出来是坏音频。
// 把"改时长"这件事的错值挡在编译期, 而不是等到录完一段才发现声音不对。
static_assert(kBufferSize % (CHANNELS * (BITS_PER_SAMPLE / 8)) == 0,
              "AUDIO_SDK_BLOCK_MS 算出的块长不是整帧, 请取能整除的时长");

const char* FormatName(AudioSdkFormat fmt) {
   switch (fmt) {
   case AudioSdkFormat::OGG:  return "OGG";
   case AudioSdkFormat::OPUS: return "OPUS";
   default:                   return "WAV";
   }
}
}   // namespace

/**
 * @brief 音频录制实现(Windows, PIMPL: winmm 全部收在 Impl 内, 不泄露到接口头)
 * @note 该类负责加载、播放、暂停、停止音频文件。
*/
struct CAudioRecorder::Impl{
   static void WaveInProc(HWAVEIN hWaveIn, UINT uMsg, DWORD_PTR dwInstanceData,
      DWORD_PTR wParam, DWORD_PTR lParam);          // 设备回调函数指针
   void OnBufferDone(WAVEHDR* hdr);                  // 缓冲区完成回调函数
   void AbortStart();                                // 启动失败/正常停止共用的收尾

   HWAVEIN   m_hWaveIn   = NULL;         // 句柄
   WAVEHDR   m_waveHdrIn[kBufferCount] = {};  // 音频缓冲区

   CWavStreamWriter m_wavWriter;
   COggStreamWriter m_oggWriter;
   COpusStreamWriter m_opusWriter;
   // 本轮走哪个落盘器。跟 m_isRecording 一样跨到音频线程上读, 所以也得是原子
   std::atomic<AudioSdkFormat> m_fmt{AudioSdkFormat::WAV};

   // 落盘路径 = 输出名 + 后缀
   std::string OutFile() const {
      const bool bEnc = m_isAencEncrypt;
      switch (m_fmt.load(std::memory_order_relaxed)) {
      case AudioSdkFormat::OGG:  return m_outputPath + (bEnc ? ".aogg" : ".ogg");
      case AudioSdkFormat::OPUS: return m_outputPath + (bEnc ? ".aopus" : ".opus");
      default:                   return m_outputPath + (bEnc ? ".aenc" : ".wav");
      }
   }

   // ---- 三种落盘器统一入口: 格式开录前定死, 音频线程只认 m_fmt ----
   AudioSdk::AudioSdkState OpenWriter(const std::string& sFile, bool bEncrypt) {
      switch (m_fmt.load(std::memory_order_relaxed)) {
      case AudioSdkFormat::OGG:  return m_oggWriter.Open(sFile.c_str(), bEncrypt);
      case AudioSdkFormat::OPUS: return m_opusWriter.Open(sFile.c_str(), bEncrypt);
      default:                   return m_wavWriter.Open(sFile.c_str(), bEncrypt);
      }
   }
   AudioSdk::AudioSdkState WritePcm(void* pcm, size_t n) {
      switch (m_fmt.load(std::memory_order_relaxed)) {
      case AudioSdkFormat::OGG:  return m_oggWriter.Write(pcm, n);
      case AudioSdkFormat::OPUS: return m_opusWriter.Write(pcm, n);
      default:                   return m_wavWriter.Write(pcm, n);
      }
   }
   uint64_t WrittenBytes() const {
      switch (m_fmt.load(std::memory_order_relaxed)) {
      case AudioSdkFormat::OGG:  return m_oggWriter.GetWritten();
      case AudioSdkFormat::OPUS: return m_opusWriter.GetWritten();
      default:                   return m_wavWriter.GetWritten();
      }
   }
   bool FinishWriter() {
      switch (m_fmt.load(std::memory_order_relaxed)) {
      case AudioSdkFormat::OGG:  return m_oggWriter.Finish();
      case AudioSdkFormat::OPUS: return m_opusWriter.Finish();
      default:                   return m_wavWriter.Finish();
      }
   }

   // 已录 PCM 字节数(【喂进去的】原始字节, 不是压出来的)
   std::atomic<size_t> m_recordedBytes{0};
   // 跨线程读写: UI 线程置位, 音频线程在 OnBufferDone 里读 → 必须原子
   std::atomic<bool> m_isRecording{false};  // 是否正在录制
   // 录制途中第一次写失败的状态码(0 = NONE)
   std::atomic<int> m_writeErr{0};
   // 设备被拔(waveIn 送来 WIM_CLOSE)时置位, 由回调线程写、调用线程读。
   std::atomic<bool> m_deviceLost{false};

   std::atomic<int> m_cbInFlight{0};     // 正在回调里跑的线程数
   std::atomic<int> m_bufsInDriver{0};   // 已挂给驱动、还没退回来的缓冲数
   bool      m_isPaused    = false;      // 是否正在暂停录制
   bool      m_isAencEncrypt = true;     // 是否加密保存(默认加密)
   AudioSdkFormat m_type = AudioSdkFormat::WAV; // 默认格式类型
   
   std::string m_outputPath = "output";

   // ---- 波形 ----
   CWaveRing m_waveRing;
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
 * @brief 退回已挂上的缓冲并关设备
 * @note 启动中途失败和正常停止, 走的是同一套收尾动作
 */
void CAudioRecorder::Impl::AbortStart(){
   if (m_hWaveIn == NULL) return;
   m_isRecording = false;

   waveInReset(m_hWaveIn);        // 停采 + 让驱动把在途缓冲全部退回来

   // 等回调跑完再动缓冲
   for (int spin = 0; spin < 1000; spin++) {
      if (m_cbInFlight.load(std::memory_order_acquire) == 0 &&
          m_bufsInDriver.load(std::memory_order_acquire) == 0)
         break;
      Sleep(1);                   // 回调只做几次拷贝, 通常等不到这一下
   }

   for (int iN = 0; iN < kBufferCount; iN++) {
      WAVEHDR& hdr = m_waveHdrIn[iN];
      if (hdr.lpData == nullptr) continue;
      if (waveInUnprepareHeader(m_hWaveIn, &hdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
         hdr.lpData = nullptr;    // 拿不回来就只摘指针不 free(兜底路径)
         continue;
      }
      delete[] hdr.lpData;
      hdr.lpData = nullptr;
   }

   waveInClose(m_hWaveIn);
   m_hWaveIn = NULL;
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

   if (p->m_isRecording.load()) {
      AUDIO_LOG_WARN(kTag, "重复开始录音(已在录, 已录 %u ms) -> DEVICE_BUSY", GetRecordedMs());
      return AudioSdk::AudioSdkState::DEVICE_BUSY;
   }

   MMRESULT res = waveInOpen(&p->m_hWaveIn, WAVE_MAPPER,
      &fmt, (DWORD_PTR)&Impl::WaveInProc,
       (DWORD_PTR)p, CALLBACK_FUNCTION);

   if (res == MMSYSERR_NODRIVER || res == MMSYSERR_BADDEVICEID) {
      AUDIO_LOG_ERROR(kTag, "waveInOpen 打不开设备 (MMRESULT=%u) -> DEVICE_NOT_FOUND",
                      static_cast<unsigned>(res));
      return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;        // 无法打开音频设备
   }
   if (res == MMSYSERR_ALLOCATED ) {
      AUDIO_LOG_ERROR(kTag, "waveInOpen 设备已被占用 (MMRESULT=%u) -> DEVICE_BUSY",
                      static_cast<unsigned>(res));
      return AudioSdk::AudioSdkState::DEVICE_BUSY;             // 设备已被占用
   }
   if (res == MMSYSERR_NOMEM) {
      AUDIO_LOG_ERROR(kTag, "waveInOpen 驱动建不出缓冲 (MMRESULT=%u) -> OUT_OF_MEMORY",
                      static_cast<unsigned>(res));
      return AudioSdk::AudioSdkState::OUT_OF_MEMORY;           // 驱动建不出内部缓冲
   }
   // 兜底: 上面没列到的错误以前会一路掉到函数末尾的 return NONE —— 等于谎报"开录成功"。
   if (res != MMSYSERR_NOERROR) {
      AUDIO_LOG_ERROR(kTag, "waveInOpen 未知失败 (MMRESULT=%u) -> PLATFORM_ERROR",
                      static_cast<unsigned>(res));
      return AudioSdk::AudioSdkState::PLATFORM_ERROR;
   }

   p->m_recordedBytes.store(0, std::memory_order_relaxed);   // 计数跟着清零
   p->m_writeErr.store(0, std::memory_order_relaxed);        // 上一轮的写失败标记不带进这一轮
   p->m_deviceLost.store(false, std::memory_order_relaxed);  // 掉线标记同理
   // 波形从头开始: 丢掉上一轮没取走的点(此时设备还没 start, 音频线程没在跑)
   p->m_waveRing.Reset();

   // 本轮走哪个写入器就在这儿定死 —— 往后的音频线程只认 m_fmt
   p->m_fmt.store(p->m_type, std::memory_order_relaxed);

   const std::string outFile = p->OutFile();
   const AudioSdk::AudioSdkState opened = p->OpenWriter(outFile, p->m_isAencEncrypt);
   if (opened != AudioSdk::AudioSdkState::NONE) {
      AUDIO_LOG_ERROR(kTag, "开录时打不开落盘文件 \"%s\" -> 状态码 %d",
                      outFile.c_str(), static_cast<int>(opened));
      p->m_isRecording = false;
      p->AbortStart();
      return opened;
   }

   // 初始化缓冲区
   for (int iN = 0; iN < kBufferCount; iN++){
      WAVEHDR& hdr = p->m_waveHdrIn[iN];
      ZeroMemory(&hdr, sizeof(WAVEHDR));
      // nothrow: 分配失败返回 NULL 而不是抛异常(BYTE 是平凡类型, 用 char 分配再按 BYTE 用)
      hdr.lpData = new (std::nothrow) char[kBufferSize];
      if (hdr.lpData == nullptr) {
         AUDIO_LOG_ERROR(kTag, "第 %d 块缓冲分配失败 (%zu 字节) -> OUT_OF_MEMORY", iN, kBufferSize);
         p->m_isRecording = false;   // 先置位: 回滚时设备可能已经在来回调了
         p->AbortStart();
         return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
      }
      hdr.dwBufferLength = static_cast<DWORD>(kBufferSize);   // 设置缓冲区大小

      // winmm 挂缓冲也要自己分配, 会失败 —— 以前两个返回值都没看
      const MMRESULT prep = waveInPrepareHeader(p->m_hWaveIn, &hdr, sizeof(WAVEHDR));
      const MMRESULT add  = (prep == MMSYSERR_NOERROR)
                              ? waveInAddBuffer(p->m_hWaveIn, &hdr, sizeof(WAVEHDR))
                              : prep;
      if (add != MMSYSERR_NOERROR) {
         AUDIO_LOG_ERROR(kTag, "第 %d 块挂到驱动失败 (MMRESULT=%u) -> %s", iN,
                         static_cast<unsigned>(add),
                         add == MMSYSERR_NOMEM ? "OUT_OF_MEMORY" : "PLATFORM_ERROR");
         p->m_isRecording = false;
         p->AbortStart();
         return (add == MMSYSERR_NOMEM) ? AudioSdk::AudioSdkState::OUT_OF_MEMORY
                                        : AudioSdk::AudioSdkState::PLATFORM_ERROR;
      }
      // 真正进了驱动队列才记账 —— 收尾时靠它判断"还有没有回调会来"
      p->m_bufsInDriver.fetch_add(1, std::memory_order_release);
   }

   p->m_isRecording.store(true, std::memory_order_release);
   const MMRESULT start = waveInStart(p->m_hWaveIn);
   if (start != MMSYSERR_NOERROR) {
      AUDIO_LOG_ERROR(kTag, "waveInStart 失败 (MMRESULT=%u) -> %s", static_cast<unsigned>(start),
                      start == MMSYSERR_NOMEM ? "OUT_OF_MEMORY" : "PLATFORM_ERROR");
      p->m_isRecording = false;
      p->AbortStart();
      return (start == MMSYSERR_NOMEM) ? AudioSdk::AudioSdkState::OUT_OF_MEMORY
                                       : AudioSdk::AudioSdkState::PLATFORM_ERROR;
   }
   AUDIO_LOG_INFO(kTag, "开始录音 -> \"%s\" (%uch %uHz %ubit %s %s)",
                  outFile.c_str(), CHANNELS, SAMPLE_RATE, BITS_PER_SAMPLE,
                  FormatName(p->m_fmt.load(std::memory_order_relaxed)),
                  p->m_isAencEncrypt ? "加密" : "明文");
   return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 暂停/继续音频录制
 */
void CAudioRecorder::PauseResumeRecording() {
   Impl* p = m_impl;
   // 检查是否正在录制
   if (!p->m_isRecording) return;
   // 写失败之后设备已经没排队缓冲了, resume 只会开一个空转的设备
   if (p->m_writeErr.load(std::memory_order_relaxed) != 0) return;
   if (p->m_isPaused) {
      waveInStart(p->m_hWaveIn);
      p->m_isPaused = false;
      AUDIO_LOG_INFO(kTag, "继续录音 (已录 %u ms)", GetRecordedMs());
   } else {
      waveInStop(p->m_hWaveIn);        // waveInStop 暂停采集，但不关闭设备
      p->m_isPaused = true;
      AUDIO_LOG_INFO(kTag, "暂停录音 (已录 %u ms)", GetRecordedMs());
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

   // 设备中途被拔(回调置的): 数据还在, 但这一轮是被打断的 —— 落完盘要如实报出去。
   // 取走即清, 不带到下一轮。
   const bool lost = p->m_deviceLost.exchange(false, std::memory_order_acq_rel);

   p->m_isRecording = false;
   p->m_isPaused = false;
   // 收回缓冲 + 关设备: 内部会等回调跑完, 所以走到这里数据已经全部落盘了
   p->AbortStart();

   const std::string outFile = p->OutFile();
   // 落盘大小
   const size_t nBytes = static_cast<size_t>(p->WrittenBytes());

   // Ogg/Opus: 排空管道 + 写 EOS 页; WAV: 回填长度 + 关文件
   const bool flushed = p->FinishWriter();
   // 录制途中的写失败(内存/磁盘)在那一刻就定了, 这里只取一次结果; 取走即清
   const int writeErr = p->m_writeErr.exchange(0, std::memory_order_acq_rel);

   if (writeErr != 0) {
      AUDIO_LOG_ERROR(kTag, "录制途中写失败, \"%s\" 只写到 %zu 字节 -> 状态码 %d",
                      outFile.c_str(), nBytes, writeErr);
      return static_cast<AudioSdk::AudioSdkState>(writeErr);
   }
   if (!flushed) {
      AUDIO_LOG_ERROR(kTag, "收尾写文件头/刷缓冲失败, \"%s\" (%zu 字节) 是残的 -> FILE_WRITE_FAILED",
                      outFile.c_str(), nBytes);
      return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
   }

   const double secs = static_cast<double>(p->m_recordedBytes.load(std::memory_order_relaxed)) /
                       (SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8));
   if (lost) {
      // 数据保住了, 但这一轮是被设备拔掉打断的 —— 如实报, 别让调用方以为录全了
      AUDIO_LOG_WARN(kTag, "停止录音(设备中途被拔, 数据不完整) -> \"%s\" (文件 %zu 字节, 时长 %.2fs)",
                     outFile.c_str(), nBytes, secs);
      return AudioSdk::AudioSdkState::DEVICE_NOT_FOUND;
   }
   AUDIO_LOG_INFO(kTag, "停止录音 -> \"%s\" (文件 %zu 字节, 时长 %.2fs)",
                  outFile.c_str(), nBytes, secs);
   return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 设备回调函数
 */
void CALLBACK CAudioRecorder::Impl::WaveInProc(HWAVEIN hWaveIn, UINT uMsg, DWORD_PTR dwInstanceData,
   DWORD_PTR wParam, DWORD_PTR lParam) {
   // 设备关了/被拔了: 驱动此后不会再送 WIM_DATA, 这一轮就断在这儿
   if (uMsg == WIM_CLOSE) {
      Impl* self = reinterpret_cast<Impl*>(dwInstanceData);
      
      if (self && self->m_isRecording.load(std::memory_order_acquire))
         self->m_deviceLost.store(true, std::memory_order_release);
      return;
   }
   if (uMsg != WIM_DATA)     // 系统预定义常量(0x3C4)，录满一个缓冲区时来一次
      return;
   Impl* self = reinterpret_cast<Impl*>(dwInstanceData);
   self->m_cbInFlight.fetch_add(1, std::memory_order_acq_rel);
   self->m_bufsInDriver.fetch_sub(1, std::memory_order_acq_rel);
   self->OnBufferDone((WAVEHDR*)wParam);
   self->m_cbInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

/**
 * @brief 缓冲区完成回调函数
 * @param hdr 指向 WAVEHDR 结构体的指针
 */
void CAudioRecorder::Impl::OnBufferDone(WAVEHDR* hdr) {

   // ---- 波形: 算好这一块的峰值, 写进环形缓冲等调用方来取 ----
   // 只在"确实还在录"时写: StopRecording 会先把 m_isRecording 置 false,
   // 于是 waveInReset 触发的收尾回调不会再往里塞数据(那时调用方可能正在收尾)。
   if (m_isRecording.load(std::memory_order_acquire) &&
       hdr->dwBytesRecorded >= sizeof(int16_t)) {
      // 传字节指针即可, ComputePeaks 内部按字节读(不依赖缓冲的 2 字节对齐)
      CWaveform::ComputePeaks(hdr->lpData, hdr->dwBytesRecorded,
                              m_waveBuf, CWaveform::kPointsPerBlock);
      // 拷进环里就结束 —— 这一行不调用任何调用方代码, 音频线程上是干净的
      m_waveRing.Push(m_waveBuf, CWaveform::kPointsPerBlock);
   }
   
   if (!m_isRecording.load(std::memory_order_acquire)) return;

   // 边录边写, 写不进说明盘满/设备出错，可看日志
   auto ret = WritePcm(hdr->lpData, hdr->dwBytesRecorded);
   if (ret != AudioSdk::AudioSdkState::NONE) {
      int iExpected = 0;
      m_writeErr.compare_exchange_strong(iExpected,
         static_cast<int>(ret),
         std::memory_order_relaxed);
      return;
   }
  
   m_recordedBytes.fetch_add(hdr->dwBytesRecorded, std::memory_order_relaxed);
   // 续缓冲前多看一眼 m_writeErr: 已经写不进去了, 再采下去也只会继续失败
   if (m_isRecording.load(std::memory_order_acquire) &&
       m_writeErr.load(std::memory_order_relaxed) == 0) {
      // 挂回去了才记账; 挂失败就等于这块退休, 计数不能凭空加回去
      if (waveInAddBuffer(m_hWaveIn, hdr, sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
         m_bufsInDriver.fetch_add(1, std::memory_order_release);
   }
}

/**
 * @brief 设置落盘容器(录制中不生效)
 * @note 录制中不生效的道理跟 SetAencEncrypt/SetOutputPath 一样: 这一轮"录成什么"应当开录前定死。
 */
void CAudioRecorder::SetFormat(AudioSdkFormat format) {
   Impl* p = m_impl;
   if (p->m_isRecording) {
      AUDIO_LOG_WARN(kTag, "录制中调 SetFormat(%d), 不生效", static_cast<int>(format));
      return;
   }
   if (format != AudioSdkFormat::WAV && format != AudioSdkFormat::OGG &&
       format != AudioSdkFormat::OPUS) {
      AUDIO_LOG_WARN(kTag, "暂不支持的格式 %d, 回退 WAV", static_cast<int>(format));
      format = AudioSdkFormat::WAV;
   }
   p->m_type = format;
   AUDIO_LOG_INFO(kTag, "落盘容器 -> %s", FormatName(format));
}

AudioSdkFormat CAudioRecorder::GetFormat() const {
   return m_impl->m_type;
}

/**
 * @brief 设置是否加密保存(录制中不生效)
 */
void CAudioRecorder::SetAencEncrypt() {
   Impl* p = m_impl;
   if (p->m_isRecording) {
      AUDIO_LOG_WARN(kTag, "录制中调 SetAencEncrypt, 不生效");
      return;   // 录制中不允许切换
   }
   p->m_isAencEncrypt = !p->m_isAencEncrypt;
   AUDIO_LOG_INFO(kTag, "加密开关 -> %s", p->m_isAencEncrypt ? "开(.aenc)" : "关(.wav)");
}

/**
 * @brief 设置落盘路径(UTF-8, 不含扩展名 —— 后缀按加密开关补)
 * @note 录制中不生效, 与 SetAencEncrypt 同一个道理: 这一轮的"要录到哪"应当开录前就定死。
 *       空指针在这里不会出现(C 接口层已经挡掉), 这里只管非空串。
 */
void CAudioRecorder::SetOutputPath(const char* utf8Path) {
   Impl* p = m_impl;
   if (p->m_isRecording) {
      AUDIO_LOG_WARN(kTag, "录制中调 SetOutputPath(\"%s\"), 不生效", utf8Path ? utf8Path : "");
      return;   // 录制中不生效
   }
   p->m_outputPath = utf8Path;
   AUDIO_LOG_INFO(kTag, "落盘路径 -> \"%s\"", utf8Path ? utf8Path : "");
}

bool CAudioRecorder::GetAencEncrypt() const {
   return m_impl->m_isAencEncrypt;
}


bool CAudioRecorder::GetIsPaused() const{
   return m_impl->m_isPaused;
}

/**
 * @brief 录制途中的写失败状态码(NONE = 没失败)
 */
AudioSdk::AudioSdkState CAudioRecorder::GetWriteError() const{
   return static_cast<AudioSdk::AudioSdkState>(m_impl->m_writeErr.load(std::memory_order_relaxed));
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
 * @brief 取走尚未读过的录音波形点(拉模式)
 * @param outMinMax 输出缓冲, 布局 [min0,max0,min1,max1,...], 需 maxPoints*2 个 float
 * @param maxPoints 本次最多取几个点
 * @return 实际取到的点数; 0 = 暂无新数据(不是错误)
 * @note 与音频线程只通过环形缓冲打交道, 不加锁。
 *       不要与 StartRecording 并发调用 —— 开始录音会重置环形缓冲。
 */
int CAudioRecorder::ReadWave(float* outMinMax, int maxPoints) {
   return m_impl->m_waveRing.Pop(outMinMax, maxPoints);
}
