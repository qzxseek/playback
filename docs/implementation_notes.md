# audio_sdk 功能实现说明

> 跨平台音频录制 / 播放 SDK：Windows（winmm）+ Android（AAudio），共享格式层与加密容器。
> 文档版本 v1.0 / 2026-10-22 / 孟源

---

## 1. 功能概述

### 1.1 背景

需要一套能在本地录制音频、回放录音、并把音频波形画到界面上的能力。难点不在"录一段声音"，而在三件事：

1. **两个平台要一致**：Windows 和 Android 的音频设备 API 完全不同，但上层业务不该写两套逻辑。
2. **录音不能越录越占内存**：长时间录音（几十分钟）不能把整段 PCM 攒在内存里。
3. **文件要有基本的保护**：录音内容不希望被任意播放器直接打开。

### 1.2 目标

| 目标 | 达成方式 |
| --- | --- |
| 一套接口跨两平台 | 平台无关的 C ABI + PIMPL 隐藏设备层 |
| 边录边写，内存恒定 | 流式落盘器，内存中只有一块固定缓冲 |
| 支持压缩格式 | WAV（无损）、Ogg Vorbis、Opus 三种落盘格式 |
| 文件带防护 | 加密容器（`.aenc` / `.aogg` / `.aopus`） |
| 界面可画波形 | 录音拉模式环形缓冲 + 播放整段降采样 |
| 调用方零依赖 | 显式加载器，不需要链接导入库 |

### 1.3 架构分层

```
┌─────────────────────────────────────────────┐
│  调用方：Win32 UI / Android App              │
├─────────────────────────────────────────────┤
│  接入层：sdk_loader（LoadLibrary + 函数表）  │
├─────────────────────────────────────────────┤
│  契约层：audio_c_api.h（extern "C"，平台无关）│
│         audio_player.h / audio_recorder.h    │
├─────────────────────────────────────────────┤
│  公共层：src/common/（格式、加密、缓冲、波形）│
├─────────────────────────────────────────────┤
│  设备层：src/windows/        src/android/    │
│          winmm                AAudio         │
└─────────────────────────────────────────────┘
```

**分层原则**：平台层分裂、公共层单源码。判断一份头归哪层的标准是——**它碰不碰 `windows.h` / `aaudio/AAudio.h`**。碰就是设备层，不碰就是公共层。

---

## 2. 需求范围

### ✅ 支持

- 录制 44100 Hz / 16 bit / 单声道音频
- 三种落盘格式：WAV、Ogg Vorbis、Opus
- 加密保存（可选，默认开）
- 录制中暂停 / 继续
- 播放 WAV / Ogg Vorbis / Opus 及其加密变体（按文件头自动识别）
- 播放跳转、暂停、进度查询
- 录音波形实时拉取、播放波形整段生成
- 录音中被拔出设备、磁盘写满等异常的状态上报

### ❌ 不支持

- 非 44100 / 16 bit / 单声道的录音（录音格式编译期固定）
- 多声道录音、采样率切换
- 实时耳返监听、低延迟对讲（块长 100 ms）
- 网络推流、断点续录
- Android 8.0（API 26）以下
- iOS / macOS / Linux

---

## 3. 技术方案与实现思路

### 3.1 技术栈选型

| 能力 | 选用 | 备选 | 理由（详见 3.2 / 3.3） |
| --- | --- | --- | --- |
| Android 设备层 | **AAudio** | OpenSL ES | 仍在维护的接口；回调签名简单；有正式错误通道 |
| Windows 设备层 | **waveIn / waveOut** | WASAPI | 代码量约为 WASAPI 的 1/3；格式转换交给系统混音器 |
| WAV 编解码 | **手写** | 第三方库 | 格式极简（44 字节头），引库反而是负担 |
| Ogg Vorbis | **libvorbis**（vorbisenc + vorbisfile） | — | 上游官方实现，仓库内自带预编译包 |
| Opus | **libopus + libopusenc** | — | libopus 只管编解码，Ogg 封装交给 libopusenc |
| Ogg 页处理 | **libogg** | — | 分页、CRC、granulepos 由它负责 |
| 加密 | **固定密钥循环 XOR + 6 字节容器前缀** | AES | 目标是"防误播/防直接打开"，不是密码学保密 |
| 波形计算 | **自实现降采样** | 第三方库 | 只需要 min/max 峰值，算法十几行 |
| 线程同步 | **原子量 + 环形缓冲** | 互斥锁 | 音频线程不能阻塞，不能加锁 |
| 日志 | **自研**（按日切文件） | — | 需要在无第三方依赖下两平台统一输出 |
| 动态加载 | **自研加载器** | 链接导入库 | 调用方可决定 DLL 位置，缺失符号能整体回滚 |

第三方库全部随仓库提供，**不需要 vcpkg，不需要联网**。

### 3.2 Android：为什么选 AAudio

- **OpenSL ES 是上一代 API**，Google 已不再为它增加新能力；
- **新的低延迟路径（MMAP）只存在于 AAudio 上**；
- 现代设备绝大多数在 8.0 以上，为 5.x/6.x/7.x 维护 OpenSL ES 分支不划算。

本项目把这个代价显式化，`audio_c_api.h` 开头有编译期检查：

```c
#elif __ANDROID_API__ < 26
  #error "audio_sdk: Android API 26 (8.0) or newer is required ..."
```

**换来了什么**

| 收益 | 说明 |
| --- | --- |
| 回调签名简单 | 一个普通 C 函数 `(stream, userData, audioData, numFrames)`；OpenSL ES 要先建引擎→混音器→播放器，每步都可能失败 |
| 格式声明直白 | `setFormat(PCM_I16)` + `setChannelCount`；OpenSL ES 要填 `SLDataFormat_PCM`，容器大小与位深分开，容易填错 |
| 错误有正式通道 | 独立错误回调能报 `AAUDIO_ERROR_DISCONNECTED` 等；OpenSL ES 设备拔出只表现为"回调不再来"，只能靠超时猜 |
| 性能可声明 | `PERFORMANCE_MODE_LOW_LATENCY` + `SHARING_MODE_EXCLUSIVE` 走 MMAP |

**付出了什么**

- API 26 以下完全不可用，`minSdkVersion` 必须 ≥ 26。
- 独占模式不保证成功，部分设备音效链不支持 MMAP，当前实现**没有 SHARED 回退**（已知缺口，见第 9 节）。

### 3.3 Windows：为什么选 waveIn / waveOut

**代价**：延迟更高（录音 100 ms × 4 块，播放 2048 帧 × 4 块 ≈ 46 ms/块）；不做重采样，格式不支持直接 `WAVERR_BADFORMAT`；没有独占模式；物理拔出 USB 声卡时 winmm 常常根本不送 `WIM_CLOSE`。

**换来了什么**

| 收益 | 说明 |
| --- | --- |
| 链路短 | `waveInOpen → 准备 4 块 WAVEHDR → waveInAddBuffer → waveInStart`；WASAPI 需要 COM 初始化、设备枚举、`GetMixFormat`、`Initialize`、事件循环 |
| 免除格式转换 | WASAPI 共享模式拿到的是 float32 立体声 48 kHz，与内部 PCM16 单声道 44.1 kHz 不匹配，得自己做重采样 + 声道映射 + 位深转换 |
| 错误码好映射 | `MMSYSERR_NODRIVER` → `DEVICE_NOT_FOUND`，`MMSYSERR_ALLOCATED` → `DEVICE_BUSY`，一张表映射完 |
| 兼容性不用管 | 从 Windows 3.1 起就有，不存在"操作系统太老不支持"的问题 |

> **一个过时的印象**：「winmm 音质差」是 XP 时代的结论，那时有 KMixer 在中间重采样。Vista 之后 KMixer 被移除，winmm 内部实际转到 WASAPI 共享模式实现。今天选 winmm 损失的只是独占和低延迟，**不是音质**。

**取舍立场**：本项目定位是录播，不是实时通话。用 3 倍的平台代码换 10 ms 延迟，在这个场景里不划算。

### 3.4 为什么两个平台方向相反

判断标准不是"新还是旧"，而是**API 处在生命周期的哪个阶段**：

| | 状态 | 结论 |
| --- | --- | --- |
| OpenSL ES | 存量兼容，不再新增能力 | 不该选 |
| AAudio | 当前推荐，新特性只在这里 | 选它 |
| winmm | 仍在维护，能力够用 | 够用，选它 |
| WASAPI | 能力更强但更重 | 能力过剩，暂不选 |

Android 是在"废弃的旧 API"和"维护中的新 API"之间选；Windows 是在"够用的简单 API"和"过剩的复杂 API"之间选。

### 3.5 总体数据流

**录音链路**

```
设备回调（音频线程）                     调用线程
─────────────────                      ────────
PCM 块到达
  ├─ 算峰值 → 写波形环 ──────────────→ ReadWave 取走
  ├─ 编码/加密 → 写文件
  └─ 缓冲挂回驱动

                              StopRecording → 回填文件头 → 返回结果
```

**播放链路**

```
                        Windows                        Android
                   ─────────────                  ─────────────
文件读取            播放线程直接读              生产者线程读进字节环
                                                     ↓
设备取数            回调只置事件                 AAudio 回调从环里拷
```

差异原因：Windows 的 `waveOutWrite` 由调用方主动喂块，文件可以直接在播放线程读；AAudio 的回调是**被设备调用**的，不能在里面做文件 IO，所以需要一条独立的生产者线程 + 字节环缓冲。

---

## 4. 核心模块详细实现

### 4.1 录音

**需求**：采集麦克风数据，边录边落盘，支持暂停，异常时如实上报。

**思路**：设备层只管把 PCM 交出来，格式交给公共层的"落盘器"处理。三个落盘器（WAV / Ogg / Opus）接口统一为 `Open / Write / Finish`，录音器按当前格式选一个。

**实现**

统一入口（`windows/audio_recorder.cpp`）：

```cpp
AudioSdk::AudioSdkState OpenWriter(const std::string& f, bool enc) {
    switch (m_fmt.load()) {
    case AudioSdkFormat::OGG:  return m_oggWriter.Open(f.c_str(), enc);
    case AudioSdkFormat::OPUS: return m_opusWriter.Open(f.c_str(), enc);
    default:                   return m_wavWriter.Open(f.c_str(), enc);
    }
}
```

**关键设计**

| 点 | 做法 | 原因 |
| --- | --- | --- |
| 缓冲块 | Windows 4 块 × 100 ms，环形挂载 | 一块在录、其余在排队/回调，避免丢数据 |
| 格式固定 | 编译期常量 44100/16/mono | 块长必须是整帧的倍数，用 `static_assert` 挡在编译期 |
| 写失败的发现 | 音频线程只置 `m_writeErr` 原子量 | 回调里不能抛异常、不能做重试 |
| 采集自停 | 写失败后不再把缓冲挂回驱动 | 继续采只会继续失败，浪费 CPU |
| 收尾责任 | 必须由调用方 `StopRecording` | 回调里不能 reset/close 设备（会死锁） |

**暂停语义**：`waveInStop` 只是暂停采集，设备不关；恢复用 `waveInStart`。这样暂停期间设备占用保持，恢复无延迟。

### 4.2 播放

**需求**：打开 WAV / Ogg / Opus 及其加密变体，播放、暂停、跳转、报进度。

**思路**：文件头决定走哪条解码路径，之后统一成"按 PCM 字节偏移取数据"的接口——进度、跳转、自然播完都不关心数据从哪来。

**实现**

格式分流只看内容不看后缀：

```
AOPU + 版本  → 加密 Opus
OpusHead     → 明文 Opus
OggS         → 明文 Vorbis
AOGG + 版本  → 加密 Vorbis
RIFF…WAVE    → 明文 WAV
AENC + 版本  → 加密 WAV
```

**Android 的字节环**：AAudio 数据回调运行在实时线程，不能读文件，所以结构是"生产者线程读文件 → 写 CByteRing → 回调从环里拷"。环容量 256 KB（约 1.5 秒 44.1k PCM）。

**自然播完判定**：Windows 在播放线程里判断"所有设备块已播完 + 文件已读完"，然后置 `m_isPlaying = false`；Android 在回调里发现到底时置 `m_reachedEnd`。

### 4.3 WAV 编解码

**需求**：无损落盘，和任意播放器兼容。

**思路**：WAV 只有 44 字节固定头 + 裸 PCM，没有压缩，没必要引库。

**实现**

头结构 `WavHeader`，`#pragma pack(1)` + `static_assert(sizeof == 44)` 双重保证布局：

| 偏移 | 字段 | 值 |
| --- | ---: | --- |
| 0 | `riffId` | `"RIFF"` |
| 4 | `riffSize` | 36 + dataSize |
| 8 | `waveId` | `"WAVE"` |
| 12 | `fmtId` | `"fmt "` |
| 16 | `fmtSize` | 16（PCM） |
| 20 | `audioFormat` | 1（PCM） |
| 22 | `numChannels` / `sampleRate` / `byteRate` / `blockAlign` / `bitsPerSample` | 按参数填 |
| 36 | `dataId` | `"data"` |
| 40 | `dataSize` | PCM 长度 |

**校验器**（`wav_validate.cpp`）比写入器严格得多：只认 PCM、`fmt` 长度必须 16、位深必须是 8 的倍数、`blockAlign` 要与声道×位深自洽、`data` 长度必须是整帧、`riffSize` 要与实际长度对得上。这是为了防止"看起来是 WAV 但解不了"的文件把播放器带进死循环。

### 4.4 Ogg Vorbis

**需求**：压缩落盘，体积约为 WAV 的 1/10。

**思路**：编码用 `libvorbisenc`（分析→分块→定型→打包→分页），解码用 `libvorbisfile`（封装 + 解码一体）。

**实现**

编码管道（`ogg_format.cpp` 的 `Drain()`）：

```
PCM → vorbis_analysis_buffer（转 float 并去交错）
    → vorbis_analysis_wrote
    → vorbis_analysis_blockout → vorbis_analysis → vorbis_bitrate_addblock
    → vorbis_bitrate_flushpacket → ogg_stream_packetin
    → ogg_stream_pageout → 写文件
```

**关键参数**：`vorbis_encode_init_vbr(vi, 1, 44100, 0.4f)` —— 可变码率，质量 0.4。

**解码**用 `ov_open_callbacks` 而不是 `ov_open(FILE*)`：后者会接管并 `fclose` 传入的 `FILE*`，和播放器自己持有的 `ifstream` 混用就是经典的 double close。用回调版才能自己管文件，也才能在读的时候顺手解密。

### 4.5 Opus

**需求**：比 Vorbis 更低码率、更低延迟的选项。

**思路**：`libopus` 只管编码解码，**不管 Ogg 封装**；封装交给 `libopusenc`（源码在 `sdk/tool/libopusenc`，随项目编译）。

**实现**

编码（`opus_format.cpp`）：

```cpp
ope_encoder_create_callbacks(&kCallbacks, this, pComments, 44100, 1, 0, &err);
ope_encoder_ctl(enc, OPUS_SET_BITRATE(24000));      // 单声道 24 kbps
ope_encoder_ctl(enc, OPE_SET_MUXING_DELAY(4800));   // 48000 域 = 100 ms
ope_encoder_write_float(...);                        // 边写边出页
```

**三个必须知道的点**

1. **采样率是输入采样率**。传 44100 进去，libopusenc 内部自动重采样到 48 kHz 再编码。
2. **解码出来永远是 48 kHz**（Opus 的规定），所以播放器要按解码器报的采样率填 `WAVEFORMATEX`，不能沿用录音的 44.1 kHz。
3. **时长推不出来**。Opus 是变长帧，只能靠末页 `granulepos` 算总采样数，还要减去开头的 `pre-skip`。

解码侧是自己拆 Ogg 页 + 调 `opus_decode`，因为 `libopusfile`（官方 Ogg Opus 读取库）不在依赖里。这块要自己处理：页索引、granulepos、pre-skip 丢弃、静音帧无数据。

### 4.6 加密容器

**需求**：录音文件不希望被任意播放器直接打开。

**思路**：不做真加密，做**混淆 + 容器封装**。原因是密钥必须内置在客户端，真加密也只是把密钥一起发出去。

**实现**

布局（以 WAV 为例）：

```
┌────────┬─────────┬──────────────┬──────────────┐
│ AENC   │ 版本 2B │ 标准 WAV 头  │ XOR 密文      │
│ 4B     │         │ 44B          │ 与明文等长     │
└────────┴─────────┴──────────────┴──────────────┘
```

- 三种容器的魔数：`AENC` / `AOGG` / `AOPU`，前缀都是 6 字节（魔数 4 + 版本 2）。
- 加密算法：16 字节密钥循环 XOR，`data[i] ^= key[(offset + i) % 16]`。
- **XOR 是自逆的**，加解密同一个函数，所以流式写入的每一块只要带上整段偏移，产物就和"整段一次加密"完全一致。
- 头部（WAV 头）是**明文**的，这样不解密也能读出格式和时长。

> ⚠️ 这不是加密。44 字节 WAV 头内容是固定的（已知明文），攻击者用"密文 ⊕ 明文"就能还原密钥流，不需要提取二进制。它的实际作用是防止文件被播放器误识别、误播放。

### 4.7 波形

**需求**：录音时实时显示波形；播放时显示整段波形。

**思路**：两套机制，因为数据来源的线程约束不同。

| | 录音 | 播放 |
| --- | --- | --- |
| 模式 | **拉**（调用方取） | **推**（回调给） |
| 原因 | 峰值在音频线程算出来，回调类型不携带线程信息，一旦调用方代码挂到音频线程上，"禁止分配内存/碰界面"只能靠自觉 | 计算在调用线程同步执行，无并发风险 |
| 粒度 | 每 100 ms 一块 → 256 点 | 整段 → 1024 点 |
| 缓冲 | 环形，容量 4096 点（约 1.6 秒） | 无（一次算完） |

**算法**：每 `N` 个采样取 min/max，归一化到 `[-1, 1]`，输出 `[min0,max0,min1,max1,...]`。

**环形缓冲**是单生产者（音频线程）、单消费者（UI 线程）的无锁结构：生产者用 release 写游标，消费者用 acquire 读。落后超过 1.6 秒时**只丢最老的波形**，主录音数据不受影响。

### 4.8 边录边写

**需求**：录 30 分钟不能占 300 MB 内存。

**思路**：开录时先写一个长度记 0 的占位头，之后每块 PCM 直接追加密文到文件末尾，停止时跳回头部回填真实长度。

**实现**

```
Open()   → 写占位头（dataSize = 0），flush
Write()  → XOR 就地加密 → 追加写 → m_written += n
Finish() → seekp 回头部 → 重写真实长度 → close
```

**内存占用**：只有一块 100 ms 的 PCM 缓冲（约 8.8 KB）在内存里，与录音时长无关。

**这里踩过的坑**：`std::ofstream::write()` 默认 `exceptions() == 0`，**写失败不抛异常，只置 badbit**。实测在断掉的管道上连写 64 次，一个 catch 都不进，只有 `good()` 变 false。所以 `Write()` 里那个 `good()` 检查是唯一能发现盘满的地方，不能拿掉。

### 4.9 C ABI 与显式加载

**需求**：调用方可以是任何语言，且能在运行时决定 DLL 位置。

**思路**：`extern "C"` 导出纯函数 + `void*` 句柄；加载器用 `LoadLibrary` + `GetProcAddress` 逐个取符号。

**实现**

```cpp
// 句柄 ↔ 指针：void* 就是 C++ 对象指针
static inline CAudioPlayer* AsPlayer(void* h) { return static_cast<CAudioPlayer*>(h); }

AUDIO_API int AudioSdk_PlayerPlayFile(void* handle, const char* utf8Path) {
    if (!handle || !utf8Path) return (int)AudioSdkState::INVALID_PARAMETER;
    return (int)AsPlayer(handle)->PlayWavFile(utf8Path);
}
```

加载器的函数指针表用 `decltype(&::AudioSdk_Xxx)` 声明，签名与 C ABI 头**自动保持一致**——改一个函数的参数类型，加载器不用跟着改。任何一个符号取不到就整体回滚，不会留下半套指针。

**为什么不用导入库**：调用方可能把 DLL 放在任意位置（安装目录、资源目录、热更新目录），导入库会在进程启动时按固定路径加载，做不到。

### 4.10 日志

**需求**：出问题时能拿到现场，且不能依赖第三方库。

**实现**：`AUDIO_LOG_DEBUG/INFO/WARN/ERROR(tag, fmt, ...)`，按日切文件（`audio_sdk_YYYYMMDD.txt`）。

| 平台 | 落盘位置 | 原因 |
| --- | --- | --- |
| Windows | exe 所在目录 | 和崩溃转储 `.dmp` 同目录，排查时一起拿 |
| Android | `/data/data/<包名>/files` | cwd 是 `/` 不可写；应用私有目录安装时就建好，不需要权限 |

每条日志立即 `flush`，并且加互斥锁——音频线程也会打日志，不加锁会串行。

---

## 5. 接口设计

### 5.1 导出函数

| 分类 | 函数 | 返回 | 说明 |
| --- | --- | --- | --- |
| 播放器 | `AudioSdk_PlayerCreate` | 句柄 | 创建 |
| | `AudioSdk_PlayerDestroy` | — | 销毁（NULL 安全） |
| | `AudioSdk_PlayerPlayFile` | 状态码 | 打开并播放 |
| | `AudioSdk_PlayerPausePlay` / `ResumePlay` / `StopPlay` | — | 传输控制 |
| | `AudioSdk_PlayerSeek` | 状态码 | 按字节偏移跳转 |
| | `AudioSdk_PlayerGetPlayPos` / `GetTotalPos` | 字节 | 进度（字节） |
| | `AudioSdk_PlayerGetPlayPosMs` / `GetTotalPosMs` | 毫秒 | 进度（时间，UI 直接用） |
| | `AudioSdk_PlayerIsPlaying` | 1/0 | 是否在播 |
| | `AudioSdk_PlayerBuildWaveform` | — | 整段波形（同步回调） |
| 录音器 | `AudioSdk_RecorderCreate` / `Destroy` | 句柄 / — | 生命周期 |
| | `AudioSdk_RecorderStart` | 状态码 | 开始录音 |
| | `AudioSdk_RecorderStop` | 状态码 | 停止并落盘 |
| | `AudioSdk_RecorderPauseResume` | — | 暂停/继续（**切换**语义） |
| | `AudioSdk_RecorderSetOutputPath` | — | 输出前缀（不含扩展名） |
| | `AudioSdk_RecorderSetFormat` / `GetFormat` | — / 序号 | 落盘格式 |
| | `AudioSdk_RecorderSetAencEncrypt` | — | 加密开关（**翻转**，默认开） |
| | `AudioSdk_RecorderGetAencEncrypt` / `GetIsPaused` | 1/0 | 状态查询 |
| | `AudioSdk_RecorderGetRecordedMs` | 毫秒 | 已录时长 |
| | `AudioSdk_RecorderGetWriteError` | 状态码 | 录制中写失败轮询 |
| | `AudioSdk_RecorderReadWave` | 点数 | 取走波形点 |
| 工具 | `AudioSdk_IsAencFile` | 1/0 | 是否加密容器 |

### 5.2 错误码

| 值 | 枚举 | 触发场景 |
| ---: | --- | --- |
| 0 | `NONE` | 成功 |
| 1 | `DEVICE_NOT_FOUND` | 设备打不开；录音中被拔出 |
| 2 | `DEVICE_BUSY` | 设备被占用；重复开始录音 |
| 3 | `FORMAT_NOT_SUPPORTED` | 魔数不匹配、非 PCM、损坏的压缩流 |
| 4 | `FILE_OPEN_FAILED` | 路径不存在、无权限、目标是目录 |
| 5 | `FILE_WRITE_FAILED` | 磁盘满、磁盘错误、收尾刷盘失败 |
| 6 | `FILE_READ_FAILED` | 文件截断、页 CRC 错 |
| 7 | `INVALID_PARAMETER` | 空句柄/空路径、未在播时 Seek |
| 8 | `OUT_OF_MEMORY` | 缓冲分配失败 |
| 9 | `PLATFORM_ERROR` | 驱动返回未归类的错误 |
| 10 | `UNKNOWN_ERROR` | 兜底 |

### 5.3 线程约定

| 操作 | 线程 |
| --- | --- |
| 录音写盘 | 音频线程（`WaveInProc` / AAudio `DataCallback`） |
| 播放读文件 | 播放线程 |
| `BuildWaveform` | **调用线程**同步执行 |
| `ReadWave` | 任意线程，但不能与 `StartRecording` 并发 |
| 异常 | **不跨越** `extern "C"` 和 C 回调边界 |

---

## 6. 文件格式设计

### 6.1 六种产物

扩展名 = 输出前缀 + 后缀，加密开关默认**开**：

| 格式 | 加密开（默认） | 加密关 | 容器 |
| --- | --- | --- | --- |
| WAV | `.aenc` | `.wav` | `AENC`/`RIFF` + 44 字节头 + PCM |
| OGG | `.aogg` | `.ogg` | `AOGG`/`OggS` + Vorbis 页 |
| OPUS | `.aopus` | `.opus` | `AOPU`/`OpusHead` + Opus 页 |

### 6.2 容器布局

| 容器 | 前缀 | 载荷 |
| --- | --- | --- |
| `.wav` | 无 | 44 字节 WAV 头 + PCM |
| `.aenc` | `AENC` + 版本 2B | 明文 WAV 头 + XOR 密文 |
| `.ogg` | 无 | Ogg 页（Vorbis） |
| `.aogg` | `AOGG` + 版本 2B | XOR 密文（Ogg 页） |
| `.opus` | 无 | Ogg 页（Opus） |
| `.aopus` | `AOPU` + 版本 2B | XOR 密文（Ogg 页） |

### 6.3 关键参数

| 参数 | 值 | 位置 |
| --- | ---: | --- |
| 采样率 / 位深 / 声道 | 44100 / 16 / 1 | `audio_types.h`（编译期固定） |
| 录音块时长 | 100 ms | `AUDIO_SDK_BLOCK_MS` |
| Vorbis 质量 | 0.4（VBR） | `ogg_format.cpp` |
| Opus 码率 | 24 kbps | `opus_format.cpp` |
| Opus 封装延迟 | 4800（48k 域 = 100 ms） | `opus_format.cpp` |
| 波形点/块 | 256 | `AUDIO_SDK_WAVE_BLOCK_POINTS` |
| 波形点/文件 | 1024 | `AUDIO_SDK_WAVE_FILE_POINTS` |
| 波形环容量 | 4096 点（≈1.6 s） | `AUDIO_SDK_WAVE_RING_POINTS` |
| 播放字节环容量 | 256 KB（≈1.5 s） | `byte_ring.h` |

---

## 7. 边界与异常处理 ⚠️

| 场景 | 处理方式 |
| --- | --- |
| 磁盘写满 | 音频线程置 `m_writeErr`，调用方轮询 `GetWriteError` 后主动 Stop；日志带 `errno` 与剩余空间 |
| 设备中途被拔 | Windows 靠 `WIM_CLOSE`，Android 靠 `AAUDIO_ERROR_DISCONNECTED`，置标记后 Stop 返回 `DEVICE_NOT_FOUND`（文件完整但数据不全） |
| 重复开始录音 | 返回 `DEVICE_BUSY`，不破坏已在录的会话 |
| 未开始就停止/暂停 | 返回 `NONE` / 无操作，不算错误 |
| 播放中换文件 | 先停当前播放再打开新文件，状态干净重置 |
| 播放中/录音中析构 | 析构先收尾落盘，再拆设备和线程 |
| 坏文件 | 校验阶段拒绝，返回 `FORMAT_NOT_SUPPORTED`，不进入设备层 |
| 扩展名造假 | 一律按文件头魔数判断，不看后缀 |
| 空块写入 | 返回 `NONE`（录音刚起步可能给 0 字节块） |
| 缓冲耗尽 | 写失败后不再挂回驱动，采集自然停止，不空转 |
| 异常跨界 | 所有平台回调内部只用返回码；`extern "C"` 边界不抛异常（会 terminate） |

**三条硬约束**（写进注释的踩坑记录）：

1. **音频回调里不分配内存、不加锁、不做文件 IO。**
2. **不在音频回调里 stop / close 流**（AAudio 会死锁，winmm 同理）。
3. **不在 C 回调边界抛异常**（UB，实际表现为 terminate）。

---

## 8. 测试与验证

自研断言框架，四个套件编成一个 exe，共 **57 个用例 / 384 条断言**。

| 套件 | 用例 | 断言 | 覆盖面 |
| --- | ---: | ---: | --- |
| 正向功能 | 12 | 155 | 三种格式往返、边录边写、录音/播放主管线、录完立刻回放 |
| 反向/非法输入 | 19 | 73 | 坏魔数、长度虚报、截断、加密壳损坏、不可写路径、写失败上报 |
| 边界条件 | 12 | 100 | 0 帧/1 帧、非标准参数、奇数填充、200 个子块、环绕满、Seek 越界 |
| 异常状态机 | 14 | 56 | 重复开始、未开始就停、播放中换文件、建销 200 次、播放中/录音中析构 |

| 验证项 | 方法 | 结果 |
| --- | --- | --- |
| WAV 往返回合 | 存盘 → 读回 → 逐字节比对 | ✅ |
| 加密容器往返 | 剥壳 → XOR 解密 → 与明文一致 | ✅ |
| 边录边写内存恒定 | 录制中检查文件增长，`Finish` 前已有数据落盘 | ✅ |
| 时长准确性 | 录 2.9 s → 落盘 `data` 255 780 字节（≈2.9 s） | ✅ |
| 播放自然结束 | 位置推进到末尾，`IsPlaying` 转假 | ✅ |
| 写失败上报 | 命名管道断管后 `Write` 必须返回失败 | ✅ |
| 资源生命周期 | 播放中/录音中析构不崩，录音会收尾落盘 | ✅ |

完整结果见 [测试报告](../tests/test_report.md)。

---

## 9. 已知问题与后续计划

### 已知问题

| 项 | 影响 | 优先级 |
| --- | --- | --- |
| Android 无 SHARED 回退 | 不支持 MMAP 独占的设备直接开流失败 | 高 |
| Android 在实时回调里同步落盘 / 编码 | 重负载下可能 xrun、丢帧 | 高 |
| C ABI 无异常保护 | `bad_alloc` 理论上会穿过 `extern "C"` | 中 |
| 加密强度等同于混淆 | 不能当保密手段用 | 说明性 |
| 句柄无类型标识 | 传错句柄类型即 UB | 低 |
| 物理拔出 USB 声卡可能不送 `WIM_CLOSE` | Windows 侧可能感知不到设备消失 | 低 |

> 完整清单（80 条，含 2 条阻断）见桌面 `review.md`。

### 后续计划

- [ ] Android 增加 SHARED 模式回退
- [ ] 录音写盘改独立线程 + 无锁队列
- [ ] 支持 48000 Hz / 双声道录制
- [ ] 增加 iOS / macOS 平台层（届时评估是否引入 Oboe / miniaudio）
- [ ] 补充 CI 与 Android 侧自动化测试

---

## 10. 相关文档

- [构建指南](build_guide.md)
- [SDK 接口文档](sdk_api.md)
- [测试报告](../tests/test_report.md)
