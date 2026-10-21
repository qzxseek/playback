# SDK 接口文档

`audio_sdk` 提供两组等价接口，按调用方语言二选一：

| 入口 | 头文件 | 适用场景 |
| --- | --- | --- |
| **C ABI**（推荐给非 C++ 调用方） | `sdk/include/audio_sdk/audio_c_api.h` | 反射调用、FFI、显式加载 DLL、C 程序 |
| **C++ 类** | `audio_player.h` / `audio_recorder.h` | 直接链接导入库的 C++ 程序 |

两者是同一份实现的薄包装，状态码与语义完全一致。本文以 C ABI 为主线，C++ 差异在各自章节末尾列出。

---

## 1. 通用约定

### 1.1 状态码

所有返回 `int` 的接口返回 `AudioSdkState` 的序号（见 `audio_types.h`）。**`0` 表示成功**，非 0 即错误。

| 值 | 枚举 | 含义 | 常见来源 |
| ---: | --- | --- | --- |
| 0 | `NONE` | 成功 | — |
| 1 | `DEVICE_NOT_FOUND` | 设备打不开 / 录制中被拔出 | `waveInOpen`/AAudio 打开失败、`WIM_CLOSE` |
| 2 | `DEVICE_BUSY` | 设备已被占用；重复开始录音 | 独占模式下被其它进程占用 |
| 3 | `FORMAT_NOT_SUPPORTED` | 文件格式不认识或不受支持 | 魔数不匹配、非 PCM、损坏的压缩流 |
| 4 | `FILE_OPEN_FAILED` | 文件打开失败 | 路径不存在、无权限、目标是目录 |
| 5 | `FILE_WRITE_FAILED` | 写入失败 | 磁盘满、磁盘错误、收尾刷盘失败 |
| 6 | `FILE_READ_FAILED` | 读取失败 | 文件截断、页 CRC 错 |
| 7 | `INVALID_PARAMETER` | 参数非法 | 空句柄/空路径、未在播时 Seek |
| 8 | `OUT_OF_MEMORY` | 内存不足 | 缓冲分配失败 |
| 9 | `PLATFORM_ERROR` | 平台层错误 | 驱动返回未归类的错误 |
| 10 | `UNKNOWN_ERROR` | 未知错误 | 兜底 |

### 1.2 路径

- 所有路径参数都是 **UTF-8**（`const char*`）。
- Windows 内部转宽字符打开文件，中文路径不会按代码页 936 解坏。
- 录制路径是**不含扩展名**的前缀，扩展名由「格式 + 加密开关」决定（见 2.2）。

### 1.3 句柄与生命周期

- `AudioSdk_*Create` 返回非空句柄，失败返回 `NULL`。
- `AudioSdk_*Destroy` 接受 `NULL`，安全无操作。
- 句柄不是线程安全的：**同一个对象不要并发调用会改状态的接口**。查询类接口（`Get*`）内部用原子量，可从其它线程读。
- 销毁正在录音/播放的对象是安全的：录音会先收尾落盘，播放会先停流。

### 1.4 线程约定

| 操作 | 约定 |
| --- | --- |
| 录制写盘 | 发生在音频线程（`WaveInProc` / AAudio `DataCallback`），不在调用线程。因此写失败**不会当场返回**，只能通过 `AudioSdk_RecorderGetWriteError` 轮询发现 |
| 播放读文件 | 在 SDK 内部线程执行，不在调用线程；文件在播完前保持打开 |
| `AudioSdk_PlayerBuildWaveform` | 在**调用线程**同步执行，无并发 |
| `AudioSdk_RecorderReadWave` | 任意线程可调，但不要与 `RecorderStart` 并发（会重置环形缓冲） |
| SDK 内部异常 | 不跨越 `extern "C"` 边界；所有错误以状态码返回 |

---

## 2. 音频参数与容器格式

### 2.1 固定参数

SDK 的录音格式是编译期常量（`audio_types.h`），**不支持运行时修改**：

| 参数 | 值 |
| --- | --- |
| 采样率 | 44100 Hz |
| 位深 | 16 bit |
| 声道 | 1（单声道） |
| 块时长 | 100 ms（`AUDIO_SDK_BLOCK_MS`） |

播放侧不受这些常量约束：WAV 直接取文件头里的采样率/声道数/位深交给声卡协商；Ogg/Opus 取解码器报出的参数（位深统一按 16 bit 解码）。

### 2.2 录制产物扩展名

扩展名 = 输出前缀 + 下表后缀。**加密开关默认开启**。

| `AudioSdkFormat` | 值 | 加密开（默认） | 加密关 |
| --- | ---: | --- | --- |
| `WAV` | 2 | `.aenc` | `.wav` |
| `OGG` | 0 | `.aogg` | `.ogg` |
| `OPUS` | 1 | `.aopus` | `.opus` |

> 加密不是加密算法，是**固定 XOR 混淆 + 6 字节容器前缀**（魔数 4 字节 + 版本 2 字节）。用于防止文件被普通播放器直接打开，不提供密码学强度。

### 2.3 容器识别

播放一律**看文件头，不看扩展名**。识别顺序：

| 前缀 | 容器 | 载荷 |
| --- | --- | --- |
| `AOPU` + 版本 | `.aopus` | 加密 Ogg Opus |
| `OpusHead` | `.opus` | 明文 Ogg Opus |
| `OggS` | `.ogg` | 明文 Ogg Vorbis |
| `AOGG` + 版本 | `.aogg` | 加密 Ogg Vorbis |
| `RIFF`…`WAVE` | `.wav` | 明文 PCM |
| `AENC` + 版本 | `.aenc` | 加密 WAV |

后缀改成 `.wav` 的 `.aenc` 文件仍会按加密处理。

---

## 3. 播放器

### 3.1 接口一览

| 函数 | 返回 | 说明 |
| --- | --- | --- |
| `void* AudioSdk_PlayerCreate(void)` | 句柄 / `NULL` | 创建播放器 |
| `void AudioSdk_PlayerDestroy(void* h)` | — | 销毁，`NULL` 安全 |
| `int AudioSdk_PlayerPlayFile(void* h, const char* utf8Path)` | 状态码 | 打开并开始播放 |
| `void AudioSdk_PlayerPausePlay(void* h)` | — | 暂停 |
| `void AudioSdk_PlayerResumePlay(void* h)` | — | 继续 |
| `void AudioSdk_PlayerStopPlay(void* h)` | — | 停止并释放设备 |
| `int AudioSdk_PlayerSeek(void* h, uint32_t posBytes)` | 状态码 | 跳转（字节偏移） |
| `uint32_t AudioSdk_PlayerGetPlayPos(void* h)` | 字节 | 已播位置 |
| `uint32_t AudioSdk_PlayerGetTotalPos(void* h)` | 字节 | 总长度 |
| `uint32_t AudioSdk_PlayerGetPlayPosMs(void* h)` | 毫秒 | 已播时长 |
| `uint32_t AudioSdk_PlayerGetTotalPosMs(void* h)` | 毫秒 | 总时长；未加载为 0 |
| `int AudioSdk_PlayerIsPlaying(void* h)` | 1 / 0 | 是否在播 |
| `void AudioSdk_PlayerBuildWaveform(void* h, AudioSdkWaveCallback cb, void* userData)` | — | 生成整段波形（同步回调） |

### 3.2 行为说明

**`PlayFile`**

- 会先停止当前播放，再打开新文件——播放中换文件是合法操作。
- 返回 `NONE` 表示**已经开始播放**，不需要再调 Start。
- 失败时播放器回到未加载状态，可直接用同一句柄再播。
- 自然播完时 `IsPlaying()` 会自动变成 0，但**没有任何事件或回调**，调用方需要轮询判断（`IsPlaying() == 0` 且 `IsPaused() == false` 即播完）。

**`Seek`**

- 未在播放时调用返回 `INVALID_PARAMETER`。
- 偏移按帧对齐（向下取整到 `nBlockAlign` 的整数倍）。
- 超出总长会被夹到末尾，负数无符号传入会被当作大数，同样夹到末尾。

**暂停语义**

`PausePlay` 停住进度但不释放设备；`StopPlay` 才释放。暂停期间波形与时长查询保持原值。

### 3.3 C++ 类差异

`CAudioPlayer` 额外提供 `GetIsPaused()` 与 `IsPaused()`，C ABI 未导出。其余签名一一对应，`PlayWavFile` 等价于 `PlayerPlayFile`（名字是历史遗留，实际通吃所有支持格式）。

---

## 4. 录音器

### 4.1 接口一览

| 函数 | 返回 | 说明 |
| --- | --- | --- |
| `void* AudioSdk_RecorderCreate(void)` | 句柄 / `NULL` | 创建录音器 |
| `void AudioSdk_RecorderDestroy(void* h)` | — | 销毁；正在录会先收尾落盘 |
| `int AudioSdk_RecorderStart(void* h)` | 状态码 | 开始录音 |
| `void AudioSdk_RecorderPauseResume(void* h)` | — | 暂停 / 继续（**切换**语义） |
| `int AudioSdk_RecorderStop(void* h)` | 状态码 | 停止并落盘 |
| `void AudioSdk_RecorderSetOutputPath(void* h, const char* utf8Path)` | — | 设置输出前缀（不含扩展名） |
| `void AudioSdk_RecorderSetFormat(void* h, int format)` | — | 设置落盘格式（录制中不生效） |
| `int AudioSdk_RecorderGetFormat(void* h)` | 格式序号 | 当前格式 |
| `void AudioSdk_RecorderSetAencEncrypt(void* h)` | — | 切换加密（**翻转**，录制中不生效） |
| `int AudioSdk_RecorderGetAencEncrypt(void* h)` | 1 / 0 | 是否加密保存 |
| `int AudioSdk_RecorderGetIsPaused(void* h)` | 1 / 0 | 是否暂停 |
| `uint32_t AudioSdk_RecorderGetRecordedMs(void* h)` | 毫秒 | 已录时长 |
| `int AudioSdk_RecorderGetWriteError(void* h)` | 状态码 | 录制途中的写失败（0 = 没失败） |
| `int AudioSdk_RecorderReadWave(void* h, float* out, int maxPoints)` | 点数 | 取走未读波形点 |
| `int AudioSdk_IsAencFile(const char* utf8Path)` | 1 / 0 / 状态码 | 是否加密容器（`NULL` 返回 7） |

### 4.2 行为说明

**`Start`**

- 重复调用返回 `DEVICE_BUSY`，不会破坏已经在录的会话。
- 失败时设备与缓冲都会回滚，可直接重试。
- 输出路径打不开会当场返回 `FILE_OPEN_FAILED` / `FILE_WRITE_FAILED`。
- 开录后文件立刻落一个占位头，录制中持续追加数据——**内存占用不随录音时长增长**。

**`Stop`**

- 未在录音时返回 `NONE`（不算错误）。
- 返回码按优先级：录制中的写失败 → 收尾刷盘失败 → 设备中途被拔 → 成功。
- `DEVICE_NOT_FOUND` 特指**设备中途被拔**：文件已完整落盘，只是这一轮数据不完整。
- `FILE_WRITE_FAILED` 有两种来源，调用方需自行区分：
  - 录制中被写失败打断（`GetWriteError` 曾非 0）：文件头正常回填，是**合法的短文件**；
  - 收尾回填/刷盘失败：文件是**残的**。

**`SetOutputPath`**

- 传空指针或空串会被忽略（保持上一次的路径）。
- 路径不含扩展名：`D:\rec\session1` + 加密开 + WAV → `D:\rec\session1.aenc`。

**`SetAencEncrypt` 是翻转，不是设置**

```c
// 想确保加密关闭:
if (AudioSdk_RecorderGetAencEncrypt(rec)) AudioSdk_RecorderSetAencEncrypt(rec);
```

默认值是**开**，所以第一次调用会把它关掉。

### 4.3 写失败的轮询模式（重要）

写盘在音频线程，失败无法当场返回。**SDK 不会自己停止录音**——写失败后它只是不再把缓冲挂回驱动，采集实际已停，但设备仍开着、已录数据仍在缓冲区里，必须由调用方停止收尾。

```c
if (AudioSdk_RecorderStart(rec) != 0) { /* 处理启动失败 */ }

for (;;) {
    if (AudioSdk_RecorderGetWriteError(rec) != 0) {
        // 磁盘满 / 磁盘出错: 数据已经写不进去了。
        // 立刻 Stop: 已录部分会落成文件, Stop 返回同一个错误码。
        AudioSdk_RecorderStop(rec);
        break;
    }
    Sleep(100);              // UI 定时器里轮询即可, 100ms 足够
    if (用户点了停止) {
        AudioSdk_RecorderStop(rec);
        break;
    }
}
```

`GetWriteError` **只读不清**；清空发生在 `Stop` 内部，取走即清，不会带进下一轮。

**为什么不能靠「已录时长不再增长」判断**：暂停时它同样不增长，两者从外部无法区分。

### 4.4 C++ 类差异

`CAudioRecorder` 与 C ABI 一一对应。`SetFormat` 接受 `AudioSdkFormat` 枚举而非 `int`。切换加密同样调用无参的 `SetAencEncrypt()`（翻转语义）。

---

## 5. 波形

SDK 提供两套互不相干的波形接口。

### 5.1 播放波形（推模式，回调）

```c
void AudioSdk_PlayerBuildWaveform(void* h, AudioSdkWaveCallback cb, void* userData);
typedef void (*AudioSdkWaveCallback)(const float* minmax, int points, void* userData);
```

- 布局：`[min0, max0, min1, max1, ...]`，已归一化到 `[-1, 1]`。
- `points` 是**本次有效**的点数，不是总点数。
- **可能回调多次**：WAV 一次给全（`AUDIO_SDK_WAVE_FILE_POINTS` = 1024 点）；Ogg/Opus 解一段报一段，`points` 从少到多长。
- 想画「从左往右长出来」，每次按 `points` 画即可；想省事也可以只处理最后一次。
- 回调在**调用线程**同步执行，可以画界面、调 JNI、分配内存。
- 未加载文件或 `cb` 为 `NULL` 时不回调。

### 5.2 录音波形（拉模式，无回调）

```c
int AudioSdk_RecorderReadWave(void* h, float* outMinMax, int maxPoints);
```

- 缓冲需 `maxPoints * 2` 个 `float`。
- 返回实际取到的点数；`0` 表示暂无新数据，**不是错误**。
- 内部环形缓冲容量 `AUDIO_SDK_WAVE_RING_POINTS` = 4096 点（约 1.6 秒）。调用方落后更久时**只丢最老的波形**，主录音数据不受影响。
- 任意线程可调，但不要与 `RecorderStart` 并发。

> 录音为什么不用回调：峰值是音频线程算出来的，而 `AudioSdkWaveCallback` 不携带任何线程信息。一旦把调用方代码挂到音频线程上，「禁止分配内存/加锁/碰界面」就只能靠自觉。SDK 干脆不给这个入口。

---

## 6. 显式加载（不链接导入库）

`sdk_loader/audio_sdk_loader.h` 提供 `LoadLibrary` + `GetProcAddress` 的封装，适用于运行时才确定 DLL 位置的场景。

加载器按用途分两个类型，各自持有自己那半边的函数指针，可以只加载需要的那个：

| 类型 | 覆盖 |
| --- | --- |
| `AudioRecSdkApi` | 录音器全部接口 |
| `AudioPlaSdkApi` | 播放器全部接口 |

```cpp
#include "audio_sdk_loader.h"

AudioRecSdkApi api;
if (!api.Load()) {                 // 默认找同目录的 audio_sdk.dll
    puts(api.LastError());
    return 1;
}
void* rec = api.RecorderCreate();
api.RecorderSetOutputPath(rec, "D:\\rec\\demo");
api.RecorderStart(rec);
// ...
api.RecorderDestroy(rec);
api.Unload();                      // 必须在销毁所有对象之后
```

- 函数指针表用 `decltype(&::AudioSdk_Xxx)` 声明，签名与 C ABI 头自动保持一致。
- `Load` 会逐个取符号，**缺任何一个就整体回滚**并返回 `false`，不会留下半套指针。
- 库名可通过 `Load(L"path\\to\\audio_sdk.dll")` 指定。
- 加载器属于接入层，不是 SDK 本体。

---

## 7. 完整示例（C）

```c
#include "audio_sdk/audio_c_api.h"
#include <stdio.h>
#include <windows.h>

int main(void) {
    void* rec = AudioSdk_RecorderCreate();
    if (!rec) return 1;

    AudioSdk_RecorderSetFormat(rec, 1);                  // 1 = OPUS
    if (AudioSdk_RecorderGetAencEncrypt(rec))            // 默认加密, 这里关掉
        AudioSdk_RecorderSetAencEncrypt(rec);
    AudioSdk_RecorderSetOutputPath(rec, "D:\\rec\\demo"); // 产物: D:\rec\demo.opus

    if (AudioSdk_RecorderStart(rec) != 0) {
        AudioSdk_RecorderDestroy(rec);
        return 1;
    }

    for (int i = 0; i < 30; ++i) {                       // 录 3 秒
        Sleep(100);
        if (AudioSdk_RecorderGetWriteError(rec) != 0) {
            printf("写盘失败, 提前收尾\n");
            break;
        }
    }

    int st = AudioSdk_RecorderStop(rec);
    printf("停止: %d, 已录 %u ms\n", st, AudioSdk_RecorderGetRecordedMs(rec));
    AudioSdk_RecorderDestroy(rec);

    // 立刻回放
    void* pl = AudioSdk_PlayerCreate();
    st = AudioSdk_PlayerPlayFile(pl, "D:\\rec\\demo.opus");
    if (st == 0) {
        Sleep(AudioSdk_PlayerGetTotalPosMs(pl) + 200);
    }
    AudioSdk_PlayerDestroy(pl);
    return st;
}
```

---

## 8. 常见坑

| 现象 | 原因 |
| --- | --- |
| 录音产物后缀不是 `.wav` | 加密默认开启，WAV 格式下产物是 `.aenc` |
| 调了 `SetAencEncrypt` 反而打开了加密 | 该接口是**翻转**，不是置位 |
| `Start` 返回 `DEVICE_BUSY` | 上一次录音还没 `Stop`；`Start` 不会自动收尾 |
| 录音正常但文件时长变成 0 | 写失败后没有 `Stop`，文件头没回填 |
| 录到一半文件名找不到 | 扩展名由格式与加密开关共同决定，见 2.2 |
| `Stop` 返回 `DEVICE_NOT_FOUND` | 设备中途被拔，文件是完整的但不完整的那一轮 |
| 播放 `.opus` 失败 | 确认文件是 Ogg Opus（`OpusHead`），不是裸 Opus 包 |
| 中文路径打开失败 | 路径必须是 UTF-8，不能是 GBK |
| `Seek` 返回 7 | 未在播放时不允许 Seek |

---

## 9. 相关文档

- [构建指南](build_guide.md)
- [测试报告](../tests/test_report.md)
- [实现说明](implementation_notes.md)
- 接口契约：[`audio_c_api.h`](../sdk/include/audio_sdk/audio_c_api.h)、[`audio_types.h`](../sdk/include/audio_sdk/audio_types.h)
