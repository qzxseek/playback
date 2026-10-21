# 音频 SDK 测试报告

## 1. 概要

| 项目 | 内容 |
| --- | --- |
| 被测对象 | `audio_sdk`（Windows x64，C++17） |
| 代码基线 | `43e53c1 结构修改` + 当前工作区未提交修改 |
| 测试日期 | 2026-10-21 |
| 测试方式 | 自动化测试，自研断言框架（`tests/include/test_common.h`），四个套件编成一个可执行文件 |
| 构建配置 | Release / x64，MSVC 19.29.30159（VS2019 BuildTools），CMake 4.4.3 |
| 运行环境 | Windows 10 家庭中文版 10.0.19045，机器具备可用声卡与麦克风 |
| 执行结果 | **57 个用例、384 条断言，全部通过；失败 0、跳过 0** |
| 总耗时 | 约 14.8 秒（含真实录音/播放） |
| 退出码 | 0 |

## 2. 结果汇总

| 套件 | 用例数 | 断言数 | 通过 | 失败 | 跳过 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 正向功能（`test_positive.cpp`） | 12 | 155 | 155 | 0 | 0 |
| 反向/非法输入（`test_negative.cpp`） | 19 | 73 | 73 | 0 | 0 |
| 边界条件（`test_boundary.cpp`） | 12 | 100 | 100 | 0 | 0 |
| 异常状态机（`test_abnormal.cpp`） | 14 | 56 | 56 | 0 | 0 |
| **合计** | **57** | **384** | **384** | **0** | **0** |

> 设备类用例（录音/播放）在无可用设备时会打印 `[skip]` 而非判失败。本次运行跳过数为 0，说明录音、播放、边录边写、录音中析构等路径均真实执行。

## 3. 覆盖范围

### 3.1 正向功能（12 个用例 / 155 条断言）

| 用例 | 验证点 |
| --- | --- |
| `TestWavRoundTrip` | 明文 WAV 存盘 → 读回 → 校验 → 头字段与 PCM 逐字节一致 |
| `TestAencRoundTrip` | 加密 `.aenc` 往返：`AENC` 魔数、6 字节前缀、剥壳后仍是合法 WAV、XOR 解回明文一致 |
| `TestExtraChunkOffsets` | `fmt` 前垫 `JUNK`、`data` 后跟 `LIST` 时仍能定位真实数据段 |
| `TestStreamPlumbing` | 流式播放地基：仅凭文件前缀验头、按块 XOR 解密；流式写与整份写字节级一致、`Finish` 前数据已陆续落盘 |
| `TestOpusRoundTrip` | `.opus` / `.aopus` 编解码往返：48 kHz 解码、时长、波形非静音、`SeekTo` 后续读 |
| `TestRecordStreamsToDisk` | 边录边写：`Open` 即落占位头，录制中数据持续增长，停止时回填长度 |
| `TestRecordPipeline` | 录音主管线：计时器、真实数据量、波形点、`.aenc` 容器合法性 |
| `TestRecordOpus` | 录音格式切到 OPUS 后，产物为 `AOPU` 壳 + Ogg Opus 流（`OpusHead` 可解析） |
| `TestPlayWav` | 明文 WAV 播放：起播、总时长、波形（流式与整份算法逐点一致）、自然播完、位置到末尾 |
| `TestPlayOpus` | `.opus` / `.aopus` 播放：解码接入播放链、取波形、播完 |
| `TestPlayAenc` | 加密 `.aenc` 播放：解密后总时长与明文一致、波形逐点一致 |
| `TestRecordThenPlay` | 闭环：录一段 → 立即用播放链打开 → 时长对得上 |

关键实测数据：

- 1 秒 44.1 kHz 单声道 PCM（96 000 字节）经 Opus 编解码后，解码采样率 48 kHz，时长约 1 秒，`SeekTo` 后仍能读到真实波形。
- 录音 2 900 ms，落盘 `data` 255 780 字节（≈ 2 900 ms），波形环产出 4 096 点。
- 500 ms 素材播放：2048 点波形与明文 PCM 整份计算结果**不一致 0 点**。
- 流式写 256 KiB：`Finish` 前已有 262 188 / 262 144 字节在盘上（仅少量停留在流缓冲）。

### 3.2 反向/非法输入（19 个用例 / 73 条断言）

| 用例组 | 验证点 |
| --- | --- |
| `TestBadRiffMagic` / `TestRiffSizeMismatch` | 错误魔数（`RIFX`、大小写、`WAVE` 起头）、`riffSize` 虚大/虚小均被拒 |
| `TestFmtSizeNot16` / `TestNonPcmFormat` | `fmt` 长度 0/14/18/40、非 PCM 编码（0/2/3/65534）均被拒 |
| `TestDataBeforeFmt` | `data` 先于 `fmt` 被拒 |
| `TestBadBitsPerSample` / `TestBadBlockAlign` | 位深非 8 倍数、`blockAlign` 与声道×位深不自洽均被拒 |
| `TestZeroFormatParams` | 声道数 0、采样率 0 被拒 |
| `TestDataNotWholeFrames` / `TestDataSizeOverflow` | 半帧残料、`data` 越界被拒 |
| `TestTruncatedSizes` / `TestHeaderOnlyTruncation` | 0/1/3/4/12/16/36/43/44 字节截断、仅剩合法头均被干净拒绝 |
| `TestAencPrefixTruncated` / `TestAencInnerCorrupt` | 加密前缀不完整被拒；外壳魔数正确但内部 WAV 头损坏也能校验出来 |
| `TestBadFileViaPlayer` | 播放器入口返回码：坏文件 `FORMAT_NOT_SUPPORTED`、`NULL` 路径 `INVALID_PARAMETER`、不存在/空串 `FILE_OPEN_FAILED` |
| `TestNameDisguise` | `.aenc` 内容套 `.wav` 后缀，仍按内容识别 |
| `TestSaveToUnwritablePath` | 目标为目录、非法文件名、`NULL` 路径均返回 `FILE_OPEN_FAILED` |
| `TestStreamWriterErrors` | 未 `Open` 就写、`Open` 失败后续写、目录不存在等状态如实上报 |
| `TestWriteFailureReported` | 底层写失败时 `Write` 必须返回失败（针对 `ofstream` 默认不抛异常的历史坑） |

### 3.3 边界条件（12 个用例 / 100 条断言）

| 用例 | 验证点 |
| --- | --- |
| `TestZeroLengthData` / `TestExactlyOneFrame` | 空数据段合法善终；恰好 1 帧不被误判 |
| `TestNonStandardFormats` | 低采样率、8 位、双声道、32 位等自洽组合不被误杀 |
| `TestOddChunkPadding` / `TestManyChunks` | 奇数长度子块的 1 字节填充对齐；200 个连续子块跳块正确 |
| `TestWaveRingWrap` / `TestWaveRingOverrun` | 波形环绕满一圈、远超一圈时只保留最新满环 |
| `TestWaveRingArgs` / `TestReadWaveBounds` | 空指针、0、负数、超容量推入、空环读波形均不越界 |
| `TestByteRing` | 写满、绕圈、跨回绕点连读、`Reset`、CAS 被抢改 |
| `TestComputePeaksBounds` | 单点、点数多于采样、空入参；归一化不越出 `[-1, 1]` |
| `TestSeekClamp` | `Seek` 到 0 / 远超总长 / 停止后，位置夹在 `[0, 总长]` |

### 3.4 异常状态机（14 个用例 / 56 条断言）

| 用例 | 验证点 |
| --- | --- |
| `TestStartTwice` | 重复开始返回 `DEVICE_BUSY`，不破坏第一次会话 |
| `TestStartWithUnwritableOutput` | 输出路径打不开时当场失败，设备资源回收干净 |
| `TestStopWithoutStart` / `TestPauseWithoutStart` | 未开始就停止/暂停/恢复不崩、不污染状态 |
| `TestPauseResumeTiming` | 暂停期间时长不推进，恢复后继续 |
| `TestReadWaveAfterStop` | 停止后仍能取到最后一批波形点，取完返回 0 |
| `TestPlayInvalidThenValid` | 坏文件失败后无残留播放态，紧接着能播好文件 |
| `TestStopWithoutPlay` | 未播放时 Stop/Pause/Resume/时长/位置查询均安全 |
| `TestPlayTwice` | 播放中换文件，总长与位置干净重置 |
| `TestPauseResumePlay` | 暂停位置冻结、恢复推进、停止后暂停标记清除 |
| `TestCreateDestroyLoop` | 建销 200 次耗时 0 ms，无明显阻塞 |
| `TestCreateDestroyAfterPlay` | 完整播放路径后反复建销不崩 |
| `TestDestroyWhilePlaying` / `TestDestroyWhileRecording` | 播放中/录音中直接析构不崩；录音中析构会收尾落盘且容器合法 |

## 4. 结论

1. 当前基线在 Windows x64 Release 下功能、边界、异常路径均通过，未发现回归。
2. 压缩格式层（Opus/Vorbis）、加密容器、流式读写、波形数据、录音/播放设备链路均有端到端验证。
3. 资源生命周期（播放中/录音中析构、200 次建销）未见崩溃或明显阻塞。

## 5. 未覆盖项与风险

| 项 | 说明 |
| --- | --- |
| C ABI / DLL 导出 | 测试把 SDK 源码静态编入测试 exe（`AUDIO_SDK_STATIC`），未覆盖 `audio_c_api.cpp` 的 `extern "C"` 接口、`dllexport` 与 `LoadLibrary/GetProcAddress` 加载路径 |
| Android | 仓库内 `sdk/src/platform/android` 为空、缺少 NDK 版 Vorbis/Opus，未构建也未运行；Android 设备层代码无自动化覆盖 |
| 设备依赖 | 无声卡/麦克风的机器上相关用例会跳过，CI 覆盖率会下降；本次本机全部实跑 |
| 进程级崩溃 | “播放中/录音中析构”若真崩会中断整个 exe，只能通过退出码与是否跑完判断；句柄/线程泄漏仅间接观察，未做专项计数 |
| 并发与长稳 | 未做多对象并发、长时间浸泡、内存/句柄增长与性能基准 |
| 平台与配置 | 仅 Windows x64 Release；Debug CRT、ARM64、其他 Windows 版本未验证 |
| 极端素材 | Opus 仅覆盖 44.1 kHz 单声道 1 秒素材；多声道、异常采样率、损坏码流未系统覆盖 |

## 6. 复现方式

```bat
:: 仓库根目录
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release --target audio_sdk_tests
build\tests\test\Release\audio_sdk_tests.exe
```

退出码为 0 且输出末尾为 `合计: 失败 0` / `全部通过` 即通过。详细构建步骤见 [构建指南](../docs/build_guide.md)。
