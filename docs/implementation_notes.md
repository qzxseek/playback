# 功能实现说明

本文记录平台层的选型依据：**Android 为什么用 AAudio 而不是 OpenSL ES，Windows 为什么用 waveIn/waveOut 而不是 WASAPI**。

不止写「为什么选它」，也写「代价是什么」和「什么时候该换」。选型没有无条件正确的答案，只有对当前任务是对的答案。

---

## 1. 总览

| | Android | Windows |
| --- | --- | --- |
| 选用 | AAudio | waveIn / waveOut（winmm） |
| 备选 | OpenSL ES | WASAPI（Core Audio） |
| 引入版本 | API 26（Android 8.0） | Windows 3.1 起 |
| 代码量（录音链路） | 约 350 行 | 约 480 行（含回退逻辑） |
| 数据获取方式 | **回调推数据**（SDK 被调用） | **线程主动喂**（SDK 调设备） |
| 典型延迟 | 低（独占 + MMAP 路径） | 中（共享模式，100ms × 4 块） |
| 格式约束 | 声明即用，硬件不支持会自动重采样 | 只接受设备原生格式，不支持就失败 |

---

## 2. Android：为什么是 AAudio

### 2.1 先澄清一个容易说错的点

「AAudio 更兼容现在的 Android 版本」——这个判断**在现代 Android 上成立，但「兼容」这个词要小心**：

- **覆盖面上，AAudio 更窄**：它只在 API 26（Android 8.0，2017 年）及以上存在；OpenSL ES 从 Android 2.3 就有了。
- **但「现在」的设备几乎都在 8.0 以上**，为 5.x/6.x/7.x 保留 OpenSL ES 分支，只会让两端代码都要维护。
- 所以真正的理由是：**OpenSL ES 是上一代 API，Google 已经不再为它增加新能力；AAudio 是它的替代者，新的低延迟路径只存在于 AAudio 上。** 选一个还在被维护的接口，而不是一个只保留存量兼容的接口。

本项目把这条写进了编译期约束，`sdk/include/audio_sdk/audio_c_api.h` 开头：

```c
#elif __ANDROID_API__ < 26
  #error "audio_sdk: Android API 26 (8.0) or newer is required ..."
```

这不是"顺手加个检查"，而是**把选型的代价显式化**：如果哪天必须支持 8.0 以下，这个 `#error` 会第一时间拦住你，逼你做决定，而不是让代码在旧设备上跑到一半才出问题。

### 2.2 换来了什么

**（1）回调签名简单**

AAudio 的数据回调就是一个普通 C 函数：

```c
aaudio_data_callback_result_t DataCallback(AAudioStream* stream, void* userData,
                                           void* audioData, int32_t numFrames);
```

OpenSL ES 要先建引擎、拿接口、建混音器、建播放器，每个对象都要 `Realize` 一步，然后从 `SLAndroidSimpleBufferQueueItf` 里拿回调。同样一件事，样板代码多出好几倍，而且**每一步都可能返回失败码**，善后路径成倍增长。

**（2）格式声明直白**

```c
AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
AAudioStreamBuilder_setSampleRate(builder, nRate);
AAudioStreamBuilder_setChannelCount(builder, p->m_channels);
```

OpenSL ES 要填 `SLDataFormat_PCM`，容器大小和位深是两个独立字段，容器 16 bit 装 16 bit 有效数据还要算掩码。这类"能填错但编译不报错"的结构体，是调试噩梦的常见来源。

**（3）错误有正式通道**

AAudio 有独立的错误回调，能直接告诉你 `AAUDIO_ERROR_DISCONNECTED`（设备被拔）、`AAUDIO_ERROR_INTERNAL` 等。OpenSL ES 没有等价物——设备被拔时通常表现为 buffer queue 回调不再来，只能靠"多久没数据"去猜。本项目的设备拔出检测就是靠这个回调，见 `sdk/src/android/audio_recorder.cpp` 的 `ErrorCallback`。

**（4）性能模式可以显式声明**

```c
AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
```

在支持的设备上会走 MMAP 低延迟路径。OpenSL ES 要拿到同等效果得走 Android 专用的扩展接口，且不保证。

### 2.3 付出了什么

**（1）API 26 以下完全不可用**

这是硬代价。要用就必须接受 `minSdkVersion 26`。

**（2）独占模式不保证成功**

代码里请求了 `AAUDIO_SHARING_MODE_EXCLUSIVE`，但不少设备的音效链不支持 MMAP 独占，`openStream` 会返回 `AAUDIO_ERROR_UNAVAILABLE`。当前实现依赖上层把这类错误映射成 `DEVICE_BUSY` / `DEVICE_NOT_FOUND`，**没有做 SHARED 回退**。这是一处已知的健壮性缺口。

**（3）流对象不是线程安全的**

`m_stream` 在调用线程（Stop/Seek）和音频线程（回调）之间共享，需要自己保证时序。winmm 那边同样的问题用 `CRITICAL_SECTION` + 计数解决，Android 这边靠"回调里只置原子标记、不关流"的约定。

### 2.4 什么时候该换

| 触发条件 | 怎么办 |
| --- | --- |
| 必须支持 Android 8.0 以下 | 上 Oboe（内部自动在 AAudio / OpenSL ES 之间选），而不是自己写两套 |
| 需要同时兼顾低延迟与老设备 | 同上，Oboe 就是为这个场景做的 |
| 只用 API 26+，但想少写平台代码 | 保持现在的直连 AAudio，不必引 Oboe |

---

## 3. Windows：为什么是 waveIn / waveOut

### 3.1 先承认代价

「实现更简单」——这句是对的，而且差距不小。但简单是有代价的，先说清楚：

- **延迟更高**：录音是 `100ms/块 × 4 块`，播放是 `2048 帧/块 × 4 块`（约 46ms/块）。做本地回放没问题，**做实时耳返监听会明显听出延迟**。
- **不做重采样**：`waveOutOpen` 只接受设备原生支持的格式，格式不匹配直接返回 `WAVERR_BADFORMAT`。代码里为此写了一层回退——`WAVE_MAPPER` 打不开就枚举所有设备逐个试，全失败才报 `FORMAT_NOT_SUPPORTED`。
- **没有独占模式**：设备被其他程序占用时只能等，或者开到共享混音里被系统重采样。
- **设备热插拔不可靠**：物理拔出 USB 声卡时，winmm 常常根本不送 `WIM_CLOSE`，只能靠心跳超时兜底。

### 3.2 换来了什么

**（1）录音链路短到可以一眼看完**

```
waveInOpen → 准备 4 块 WAVEHDR → waveInAddBuffer → waveInStart
                                    ↑                      ↓
                              回调里 Write 完再挂回去 ← WIM_DATA
```

就这些。WASAPI 做同样的事要：

```
CoInitializeEx → IMMDeviceEnumerator → GetDefaultAudioEndpoint
  → Activate(IID_IAudioClient) → GetMixFormat → Initialize
  → GetService(IID_IAudioCaptureClient) → 事件驱动循环
  → GetBuffer / ReleaseBuffer
```

每一步都是 COM 接口调用，都要处理 `HRESULT`，还要保证 `CoUninitialize` 与之配对，并决定线程套间模型。

**（2）共享模式下的格式陷阱，winmm 帮你挡了一半**

`GetMixFormat` 返回的通常是 **float32 立体声 48kHz**，而本 SDK 内部是 **PCM16 单声道 44100Hz**。走 WASAPI 共享模式，就得自己做格式转换（重采样 + 声道映射 + 位深），否则声音会变调。这是一整块独立的工作量，winmm 把它交给了系统混音器。

**（3）错误码好映射**

`MMRESULT` 就是一个整数枚举：`MMSYSERR_NODRIVER` → `DEVICE_NOT_FOUND`，`MMSYSERR_ALLOCATED` → `DEVICE_BUSY`，`MMSYSERR_NOMEM` → `OUT_OF_MEMORY`。一张表就映射完了。`HRESULT` 虽然也有规律，但要配合 COM 对象状态一起判断，映射表和善后逻辑都更绕。

**（4）一个常被误解的老印象**

「winmm 音质差」是 Windows XP 时代的结论，那时有 KMixer 在中间做重采样。**Vista 之后 KMixer 被移除，winmm 内部实际上是转到 WASAPI 共享模式实现的**。所以今天选择 winmm，损失的只是"独占 / 低延迟"这些高级能力，不是音质。

### 3.3 这个项目的取舍成立吗

成立，因为本 SDK 的定位是**录播**（录音 + 本地回放），不是实时通话、不是乐器输入、不是耳返监听：

- 录音块长 100ms 是为了和波形推送节奏对齐（`AUDIO_SDK_BLOCK_MS`），不是为了延迟优化。
- 播放一次只服务一个本地文件，双向延迟不构成体验问题。
- 真要做低延迟，瓶颈不会只在 API——还要重做缓冲策略、加时间戳、处理 xrun。

**用 3 倍的平台代码换 10ms 的延迟，在这个场景里不划算。**

### 3.4 什么时候该换

| 触发条件 | 怎么办 |
| --- | --- |
| 需要耳返监听 / 实时对讲（延迟 < 30ms） | 换 WASAPI 独占模式，块长降到 10ms 级 |
| 需要每应用音量、会话管理 | 换 WASAPI（`ISimpleAudioVolume`） |
| 需要可靠的热插拔检测 | 换 WASAPI（`IMMNotificationClient`） |
| 只是继续做录播 | 保持现状 |

---

## 4. 为什么两个平台的方向相反

看起来矛盾：Android 用了**新的** API，Windows 用了**老的** API。

原因是判断标准不是「新还是旧」，而是**这个 API 处在生命周期的哪个阶段**：

| | OpenSL ES | AAudio |
| --- | --- | --- |
| 状态 | 存量兼容，不再新增能力 | 当前推荐，新特性只在这里 |
| 结论 | **不该选** | **选它** |

| | winmm | WASAPI |
| --- | --- | --- |
| 状态 | 仍在维护，功能覆盖够用 | 更底层，能力更强但更重 |
| 结论 | **够用，选它** | 能力过剩，暂不选 |

一句话：**Android 是在"废弃的旧 API"和"维护中的新 API"之间选；Windows 是在"够用的简单 API"和"过剩的复杂 API"之间选。**

---

## 5. 为什么不直接用现成的跨平台封装

Oboe（Android）、miniaudio（两端通吃）、PortAudio 都能省掉这两套平台代码。没选它们的原因是：

1. **依赖成本**：本 SDK 已经因为 Vorbis / Opus 带了两套预编译库，再加一层封装会让构建链路更长，出错时定位更难。
2. **可控性**：出问题时能直接看到 `waveInOpen` 返回了哪个 `MMRESULT`，而不是穿过两层封装猜。
3. **范围匹配**：用到的能力只是"开流 → 拿 PCM → 关流"，不需要封装层提供的设备枚举、格式协商、重采样等一大片功能。

如果将来要同时支持 iOS / macOS / Linux，这个结论会反转——那时手写四套平台层才是真正的浪费。

---

## 6. 两平台共同遵守的三条硬约束

这些不是风格偏好，是踩过之后写进注释的：

1. **音频回调里不分配内存、不加锁、不做文件 IO。** 波形峰值缓冲在对象构造时就分配好；写盘失败只置原子标记，不在回调里重试或抛异常。
2. **异常不跨越 `extern "C"` 边界，也不跨越 C 回调边界。** 所有平台回调内部用返回码，SDK 对外统一用 `AudioSdkState`。
3. **不在音频回调里 stop / close 流。** AAudio 的错误回调里直接关流会死锁，Windows 的 `WIM_CLOSE` 里关设备同理。回调只置标记，收尾一律交给调用线程。

---

## 7. 相关文档

- [构建指南](build_guide.md)
- [SDK 接口文档](sdk_api.md)
- [测试报告](../tests/test_report.md)
