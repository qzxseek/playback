# 构建指南

本文覆盖本仓库三块可独立构建的内容：SDK 动态库、自动化测试、Windows 演示程序。仓库根目录的 CMake 只负责 SDK 与测试；演示程序有独立的 CMake 工程。

## 1. 环境要求

| 项     | 要求                    | 已验证版本                               |
| ----- | --------------------- | ----------------------------------- |
| 操作系统  | Windows x64           | Windows 10 家庭中文版 10.0.19045         |
| 编译器   | MSVC，支持 C++17         | MSVC 19.29.30159（VS2019 BuildTools） |
| CMake | ≥ 3.16                | 4.4.3                               |
| 构建系统  | Visual Studio 生成器（推荐） | “Visual Studio 16 2019” / x64       |
| 音频设备  | 录音/播放用例需要；无设备时自动跳过    | 本机有声卡与麦克风                           |

第三方库**已随仓库提供**，不需要安装 vcpkg，也不需要联网：

- Vorbis / Ogg：`sdk/src/platform/windows/vorbis/`（头文件、`.lib`、CMake 包配置）
- Opus：`sdk/src/platform/windows/opus/`
- libopusenc：源码在 `sdk/tool/libopusenc/`，由 CMake 直接编译

## 2. 目录与构建入口

```
exam_sdk/
├── CMakeLists.txt          # 根工程：先 sdk/，再 tests/
├── sdk/                    # SDK 源码与 CMakeLists.txt（产出 audio_sdk.dll）
│   ├── include/audio_sdk/  # 公开头文件（C++ 类接口 + C ABI）
│   ├── logging/            # 日志模块
│   ├── src/common/         # 平台无关：WAV / AENC / OGG / Opus / 波形 / 环形缓冲
│   ├── src/windows/        # 平台层：waveOut / waveIn
│   ├── src/android/        # 平台层：AAudio
│   └── tool/libopusenc/    # .opus 封装（源码编译）
├── tests/                  # 与 sdk/ 平级的测试工程（仅 Windows）
├── sdk_loader/             # 动态加载器（头文件库，接入层）
└── windows_app/            # Win32 演示程序，独立 CMake 工程
```

## 3. 构建 SDK 与测试（根工程）

在**仓库根目录**执行：

```bat
cmake -S . -B build
cmake --build build --config Release
```

只构建某一部分时加目标名：

```bat
cmake --build build --config Release --target audio_sdk        :: 只出 DLL
cmake --build build --config Release --target audio_sdk_tests  :: 只出测试
```

> 使用 VS2022 时把生成器换成 `"Visual Studio 17 2022"`。生成器必须与本机实际安装的 Visual Studio / Build Tools 版本一致。

### 产物位置

| 产物      | 路径                                                                 |
| ------- | ------------------------------------------------------------------ |
| SDK 动态库 | `build/sdk/Release/audio_sdk.dll`                                  |
| 导入库     | `build/sdk/Release/audio_sdk.lib`                                  |
| 调试符号    | `build/sdk/Release/audio_sdk.pdb`                                  |
| 测试程序    | `build/tests/test/Release/audio_sdk_tests.exe`                     |
| 发布副本    | `windows_app/resource/audio_sdk.dll`、`audio_sdk.pdb`（构建 SDK 后自动复制） |

## 4. 运行测试

```bat
build\tests\test\Release\audio_sdk_tests.exe
```

- 退出码 `0` 表示全部通过；末尾会打印 `合计: 失败 N`。
- 从命令行/脚本运行时程序不会暂停；双击启动时会在结束前等待回车。
- 录音/播放相关用例在无可用设备时打印 `[skip]`，不算失败。
- 测试把 SDK 源码直接编入 exe（`AUDIO_SDK_STATIC`），不依赖已部署的 DLL，因此可单独验证源码。
- 测试工程必须在仓库根目录配置：`tests/CMakeLists.txt` 依赖 `sdk/CMakeLists.txt` 通过 `PARENT_SCOPE` 交出的源文件清单，单编 `tests/` 会直接报错。

测试覆盖与最近一次结果见 [测试报告](../tests/test_report.md)。

## 5. 构建 Windows 演示程序

演示程序通过 `LoadLibrary` 显式加载 `audio_sdk.dll`，不链接导入库，因此需要 DLL 与 exe 同目录（构建脚本会自动复制）。

```bat
cmake -S windows_app -B build_app -G "Visual Studio 16 2019" -A x64
cmake --build build_app --config Release
```

产物：`build_app/Release/audio_win_app.exe`。

注意构建顺序：

1. 先在根目录构建 `audio_sdk`，得到 DLL 并复制到 `windows_app/resource/`；
2. 再构建 `windows_app`，其 POST_BUILD 会把 `resource/audio_sdk.dll` 复制到 exe 旁。

## 6. Debug 构建

把 `--config Release` 换成 `--config Debug` 即可：

```bat
cmake --build build --config Debug
cmake --build build_app --config Debug
```

产物分别在 `build/sdk/Debug/`、`build/tests/test/Debug/`、`build_app/Debug/`。

> ⚠️ SDK 的 POST_BUILD 会把**当前配置**的 DLL/PDB 复制到 `windows_app/resource/`。构建 Debug 会覆盖该目录下的 Release 版本；发布前请重新做一次 Release 构建。

## 7. Android 交叉编译（未验证）

`CMakeLists.txt` 已包含 `ANDROID` 分支（AAudio 平台层 + `libaudio_sdk.so`），但仓库当前**没有** `sdk/src/platform/android/` 下的 Vorbis/Opus 预编译包，路径为空：

```
sdk/src/platform/android/vorbis/lib/cmake/Vorbis
sdk/src/platform/android/opus/lib/cmake/opus
```

因此 Android 构建前需要：

1. 安装 Android NDK（含 `build/cmake/android.toolchain.cmake`）；
2. 用 NDK 编译 Vorbis / Ogg / Opus，把安装结果放到上述两个目录（需带 CMake 包配置）；
3. 再执行：

```bat
cmake -S . -B build_android -G Ninja ^
  -DCMAKE_TOOLCHAIN_FILE=%ANDROID_NDK%/build/cmake/android.toolchain.cmake ^
  -DANDROID_ABI=arm64-v8a ^
  -DANDROID_PLATFORM=android-26 ^
  -DCMAKE_BUILD_TYPE=Release
cmake --build build_android
```

产物为 `libaudio_sdk.so`。`tests/CMakeLists.txt` 在非 Windows 平台直接 `return()`，测试不会参与 Android 构建。

## 8. 常见问题

| 现象                                             | 原因与处理                                                                                                       |
| ---------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| `could not find any instance of Visual Studio` | 生成器版本与本机安装不符，或只装了空的 VS 目录。确认已安装“使用 C++ 的桌面开发”工作负载，并改用实际版本的生成器                                               |
| `tests: 请从仓库根目录构建`                             | 直接配置了 `tests/` 目录。请从仓库根目录执行 `cmake -S . -B build`                                                           |
| `find_package(Vorbis/Opus) ... REQUIRED` 失败    | 仓库内 `sdk/src/platform/windows/{vorbis,opus}` 缺失或被移动。恢复该目录；SDK 与测试的 `CMAKE_PREFIX_PATH` 都由 CMake 自动追加，无需手工设置 |
| 运行 exe 报缺少 `audio_sdk.dll`                     | 把 `build/sdk/<Config>/audio_sdk.dll` 复制到 exe 同目录；`windows_app` 正常构建会自动完成                                    |
| 测试输出中文乱码                                       | 程序运行时已切到 UTF-8 代码页，编译也启用了 `/utf-8`。若终端仍乱码，改用 Windows Terminal 或 `chcp 65001`                                |
| 测试里出现 `[skip]`                                 | 机器没有可用声卡/麦克风，属预期降级，不代表 SDK 失败                                                                               |
| 链接出现 CRT 冲突/`LNK4098`                          | 本项目自带的 `-static-md` 预编译库与 `/MD` 匹配，不要混用其它 CRT 变体的 Vorbis/Opus                                               |

## 9. 构建通过检查清单

- [ ] 根工程配置成功，输出 `Build files have been written to ...`
- [ ] `audio_sdk.dll`、`.lib`、`.pdb` 出现在 `build/sdk/Release/`
- [ ] `audio_sdk_tests.exe` 运行后退出码为 0，输出 `全部通过`
- [ ] `windows_app/resource/audio_sdk.dll` 已更新
- [ ] `audio_win_app.exe` 能启动并加载 DLL
