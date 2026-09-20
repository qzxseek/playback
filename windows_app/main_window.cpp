/* @Created On : 2026/9/4
   @Author : 孟源
   @note : Win32 音频 UI 主窗口实现
*/
#include "include/main_window.h"

#include <cstring>      // std::memmove(滚动窗口批量左移)
#include <iterator>     // std::size(取数组元素个数, 传给 swprintf_s 做容量)


// g_pMain 声明在 main.cpp(WinMain 里 new 出来并赋值)
// 静态 WndProc 需要它把消息转发回实例
extern CMainWindows* g_pMain;

// 宽字符路径 → UTF-8(设备层 PlayWavFile 现为跨平台 UTF-8 接口, 传给它前要转)
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

// UTF-8 → 宽字符(加载失败原因是窄字符, 弹窗要宽字符)
static std::wstring Utf8ToWide(const char* utf8)
{
    if (!utf8 || !*utf8)
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 1)
        return {};
    std::wstring wide(static_cast<size_t>(len) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wide[0], len);
    return wide;
}

CMainWindows::CMainWindows(){
    // 显式加载: 只要一个 .dll, 不要导入库(.lib); 换编译器/版本也不受 C++ ABI 影响
    if (!m_api.Load()){
        MessageBoxW(nullptr, Utf8ToWide(m_api.LastError()).c_str(),
                    L"加载 audio_sdk.dll 失败", MB_OK | MB_ICONERROR);
        return;                       // 句柄保持 nullptr; CreateControls 里会把音频按钮禁掉
    }

    // 只创建一次, 全程复用(对象里存着设备/缓冲状态, 不能每次操作都新建)
    m_recorderHandle = m_api.RecorderCreate();
    m_playerHandle   = m_api.PlayerCreate();
    if (!m_recorderHandle || !m_playerHandle)
        MessageBoxW(nullptr, L"创建录音/播放对象失败(内存不足?)",
                    L"音频 SDK", MB_OK | MB_ICONERROR);

    // 录音落盘路径: 显式设成 exe 目录下的 output(绝对路径)。
    // 不设的话 SDK 用相对路径 "output", 那要由【当前工作目录】解析 —— 从别的目录启动本程序
    // 就会落到别处(在 Android 上更是直接失败, 因为工作目录 / 不可写)。
    // 路径【不带扩展名】, SDK 按加密开关补 .aenc / .wav。
    if (m_recorderHandle){
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0){
            std::wstring base(exePath);
            const size_t slash = base.find_last_of(L'\\');
            base.resize(slash == std::wstring::npos ? 0 : slash + 1);   // 砍掉 exe 文件名, 留下目录
            base += L"output";          // 仍叫 output → 停止录音那句提示文案依然成立
            m_api.RecorderSetOutputPath(m_recorderHandle, WideToUtf8(base).c_str());
        }
    }
}

CMainWindows::~CMainWindows(){
    if (m_isRecording){
        AudioStartStopRec();
    }
    if (m_isPlaying){
        AudioStartStopPlay();
    }
    // 先销毁 dll 里的对象, 再卸 dll —— 顺序反了就是往已卸载的代码里跳
    if (m_api.hModule){
        m_api.RecorderSetWaveCallback(m_recorderHandle, nullptr, nullptr);
        m_api.RecorderDestroy(m_recorderHandle);
        m_api.PlayerDestroy(m_playerHandle);
        m_recorderHandle = nullptr;
        m_playerHandle   = nullptr;
        m_api.Unload();
    }
}

/**
 * @brief 毫秒 → "MM:SS.d" 文本(如 65000ms → 01:05.0)
 * @param out 输出缓冲(建议至少 16 字符: DWORD 拉满时 "71582:47.2" + 结尾 NUL)
 * @param cch out 的容量(字符数, 含结尾 NUL) —— 用 std::size(out) 传
 * @param ms 毫秒
 */
static void FormatMs(wchar_t* out, size_t cch, DWORD ms){
    swprintf_s(out, cch, L"%02u:%02u.%u",
               (UINT)(ms / 60000),            // 分
               (UINT)((ms / 1000) % 60),      // 秒
               (UINT)((ms / 100) % 10));      // 十分之一秒
}

/**
 * @brief 录音波形回调 —— 跑在【音频线程】!
 * @param minmax 峰值对 [min0,max0,min1,max1,...], 已归一化到 [-1,1]
 * @param points 点数
 * @param userData 注册时传进来的 this
 */
void CMainWindows::OnWaveFromAudio(const float* minmax, int points, void* userData){
    if (!minmax || points <= 0) return;
    auto* self = static_cast<CMainWindows*>(userData);
    if (!self) return;

    // 防御: 环形缓冲只有 WAVE_RING_POINTS 格, 一次推来的点数不能超过它,
    // 否则后面的点会绕回去盖掉前面的(下标被取模保护, 不会越界, 但数据是乱的)。
    // 当前契约是每块 256 点、环形 4096 格, 所以正常永远走不到这个分支;
    // 这里是把不变量写出来 —— 万一有人改了 AUDIO_SDK_WAVE_BLOCK_POINTS 也不出事。
    if (points > WAVE_RING_POINTS) points = WAVE_RING_POINTS;

    // 写环形缓冲: 生产者只动 m_waveWritePos, 消费者只动 m_waveReadPos
    int iWrite = self->m_waveWritePos.load(std::memory_order_relaxed);
    for (int i = 0; i < points; ++i){
        self->m_waveRing[iWrite][0] = minmax[i * 2];        // min
        self->m_waveRing[iWrite][1] = minmax[i * 2 + 1];    // max
        iWrite = (iWrite + 1) % WAVE_RING_POINTS;                // 满了就绕回, 覆盖最老的
    }
    // release: 保证上面的写入对取到该值的消费者可见
    self->m_waveWritePos.store(iWrite, std::memory_order_release);

    // 通知 UI 线程来取。用 Post(异步)不用 Send —— Send 会阻塞音频线程等 UI 处理完
    ::PostMessageW(self->m_hwnd, WM_WAVE_DATA, 0, 0);
}

/**
 * @brief 文件波形回调 —— 在【调用线程】(这里是 UI 线程)同步执行, 无并发
 * @param minmax 峰值对
 * @param points 点数
 * @param userData 注册时传进来的 this
 */
void CMainWindows::OnWaveFromFile(const float* minmax, int points, void* userData){
    if (!minmax || points <= 0) return;
    auto* self = static_cast<CMainWindows*>(userData);
    if (!self) return;

    const int n = points < AUDIO_SDK_WAVE_FILE_POINTS ? points : AUDIO_SDK_WAVE_FILE_POINTS;
    for (int i = 0; i < n; ++i){
        self->m_fileWave[i][0] = minmax[i * 2];
        self->m_fileWave[i][1] = minmax[i * 2 + 1];
    }
    self->m_fileWaveCount = n;
}

/**
 * @brief UI 线程: 把环形缓冲里尚未消费的点并进录音滚动窗口
 * @note 只有 UI 线程调用, 所以 m_waveReadPos / m_recWave 不需要同步
 */
void CMainWindows::ConsumeWaveRing(){
    const int iWrite = m_waveWritePos.load(std::memory_order_acquire);

    // 本次能取到多少个点(环形, 可能绕了一圈)
    int avail = iWrite - m_waveReadPos;
    if (avail < 0) avail += WAVE_RING_POINTS;
    if (avail == 0) return;

    // 生产得太快、把环形挤满时, 只取最新的 REC_WAVE_POINTS 个(丢掉更老的)
    if (avail > REC_WAVE_POINTS){
        m_waveReadPos = (iWrite - REC_WAVE_POINTS + WAVE_RING_POINTS) % WAVE_RING_POINTS;
        avail = REC_WAVE_POINTS;
    }

    // 窗口放不下的老点先丢掉: 一次 memmove 把保留的部分挪到开头。
    // 这样比"每来一个点就挪一格"省掉大量重复拷贝。
    const int iTotal = m_recWaveCount + avail;
    if (iTotal > REC_WAVE_POINTS){
        int drop = iTotal - REC_WAVE_POINTS;
        // 保证 drop 不超出已有数据, 下面 keep 才是非负的。
        // (由上面 avail <= REC_WAVE_POINTS 和 m_recWaveCount <= REC_WAVE_POINTS
        //  已经能推出 drop <= m_recWaveCount, 这里写成显式钳制, 免得依赖那个推导)
        if (drop > m_recWaveCount) drop = m_recWaveCount;

        const int iKeep = m_recWaveCount - drop;
        if (iKeep > 0)
            std::memmove(m_recWave, m_recWave + drop,
                         sizeof(m_recWave[0]) * static_cast<size_t>(iKeep));
        m_recWaveCount = iKeep;
    }

    // 新点接到末尾
    for (int i = 0; i < avail; ++i){
        m_recWave[m_recWaveCount][0] = m_waveRing[m_waveReadPos][0];
        m_recWave[m_recWaveCount][1] = m_waveRing[m_waveReadPos][1];
        ++m_recWaveCount;
        m_waveReadPos = (m_waveReadPos + 1) % WAVE_RING_POINTS;
    }
}

/**
 * @brief WM_CREATE: 记录主窗口句柄并创建全部控件
 * @param hwnd 主窗口句柄
 */
void CMainWindows::OnCreate(HWND hwnd){
    m_hwnd = hwnd;
    CreateControls(hwnd);
    SetTimer(hwnd, TIMER_PROGRESS, TIMER_INTERVAL_MS, NULL);   // 启动进度心跳
}

/**
 * @brief 创建全部子控件
 * @param hwnd 主窗口句柄
 */
void CMainWindows::CreateControls(HWND hwnd){
    HINSTANCE hInst = GetModuleHandle(NULL);

    // 录音区
    m_hBtnRec_Start_Stop = CreateWindowEx(0, L"BUTTON", L"开始录音",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 10, 100, 26,
        hwnd, (HMENU)(INT_PTR)BTN_RECORD_START_STOP, hInst, NULL);
    m_hBtnRecPause = CreateWindowEx(0, L"BUTTON", L"暂停录音",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 116, 10, 100, 26,
        hwnd, (HMENU)(INT_PTR)BTN_RECORD_PAUSE, hInst, NULL);
    m_hChkEnc = CreateWindowEx(0, L"BUTTON", L"加密",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 222, 10, 80, 26,
        hwnd, (HMENU)(INT_PTR)BTN_ENCRYPT, hInst, NULL);
    m_hLblRecTime = CreateWindowEx(0, L"STATIC", L"录音时长: 00:00.0",
        WS_CHILD | WS_VISIBLE | SS_RIGHT, 306, 14, 284, 18,
        hwnd, (HMENU)(INT_PTR)IDC_LBL_REC_TIME, hInst, NULL);

    // 播放区 
    m_hBtnOpen = CreateWindowEx(0, L"BUTTON", L"打开文件",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 42, 100, 26,
        hwnd, (HMENU)(INT_PTR)BTN_OPEN_FILE, hInst, NULL);
    m_hBtnPlay_Start_Stop = CreateWindowEx(0, L"BUTTON", L"播放",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 116, 42, 80, 26,
        hwnd, (HMENU)(INT_PTR)BTN_START_STOP_PLAY, hInst, NULL);
    m_hBtnPlayPause = CreateWindowEx(0, L"BUTTON", L"暂停",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 202, 42, 90, 26,
        hwnd, (HMENU)(INT_PTR)BTN_PLAY_PAUSE, hInst, NULL);

    // 时间/状态文字(进度条右侧)
    m_hLblTime = CreateWindowEx(0, L"STATIC", L"00:00.0 / 00:00.0",
        WS_CHILD | WS_VISIBLE | SS_RIGHT, 306, 44, 284, 22,
        hwnd, (HMENU)(INT_PTR)IDC_LBL_TIME, hInst, NULL);

    // 初始化还没开始录音/播放
    EnableWindow(m_hBtnRecPause, FALSE);
    EnableWindow(m_hBtnPlay_Start_Stop, FALSE);
    EnableWindow(m_hBtnPlayPause, FALSE);

    // dll 没加载成功: 音频相关按钮全禁掉, 避免点了没反应还去调空函数指针
    if (!SdkReady()){
        EnableWindow(m_hBtnRec_Start_Stop, FALSE);
        EnableWindow(m_hChkEnc, FALSE);
        return;
    }
    // 勾选框初始状态 = 录音器真实的加密开关(默认开)。
    SendMessageW(m_hChkEnc, BM_SETCHECK,
                 m_api.RecorderGetAencEncrypt(m_recorderHandle) ? BST_CHECKED : BST_UNCHECKED, 0);
}

/**
 * @brief 窗口过程(静态成员, 注册给系统的回调)
 *   转发到 g_pMain 实例的 HandleMessage; g_pMain 为空时走默认处理
 * @param hwnd 主窗口句柄
 * @param msg 消息类型
 * @param wParam 消息参数1
 * @param lParam 消息参数2
 * @return LRESULT 处理结果
 */
LRESULT CALLBACK CMainWindows::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
    if (g_pMain)
        return g_pMain->HandleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/**
 * @brief 实例消息处理: 这里是真正能访问成员/控件句柄的地方
 * @param hwnd 主窗口句柄
 * @param msg 消息类型
 * @param wParam 消息参数1
 * @param lParam 消息参数2
 * @return LRESULT 处理结果
 */
LRESULT CMainWindows::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
    switch (msg){
       
        case WM_CREATE:                      // WM_CREATE: 记录主窗口句柄并创建全部控件
        OnCreate(hwnd);
        return 0;

        case WM_COMMAND: {
            const int iId = LOWORD(wParam);
            if (HIWORD(wParam) == BN_CLICKED)
            OnCommand(iId);
        return 0;
    }

        case WM_TIMER:                       // 进度心跳
            if (wParam == TIMER_PROGRESS)
                OnTimerTick();
            return 0;

        case WM_WAVE_DATA:                   // 音频线程推来了新波形, 在这里(UI 线程)取走
            ConsumeWaveRing();
            InvalidateWave();
            return 0;

        case WM_LBUTTONDOWN:                 // 进度条上按下: 进入"预览拖动"
            if (m_isPlaying){
                int cx = (short)LOWORD(lParam);          // 客户区 x
                RECT rc = ProgressRect();
                if (cx >= rc.left && cx <= rc.right && m_playTotalBytes > 0){
                    m_dragging = true;
                    m_playPosBytes = ClampToBytes(cx);   // 预览位置
                    UpdateProgressUI();
                    SetCapture(hwnd);                    // 按住期间持续收到鼠标消息
                }
            }
            return 0;

        case WM_MOUSEMOVE:                   // 按住拖动: 只更新预览, 不真跳
            if (m_dragging && (wParam & MK_LBUTTON)){
                m_playPosBytes = ClampToBytes((short)LOWORD(lParam));
                UpdateProgressUI();
            }
            return 0;

        case WM_LBUTTONUP:                   // 松手: 结束预览, 真正跳转到松手位置
            if (m_dragging){
                m_dragging = false;
                ReleaseCapture();
                if (m_isPlaying){
                    m_api.PlayerSeek(m_playerHandle, m_playPosBytes);          // 点哪跳哪 / 拖到哪跳到哪
                    m_playPosBytes = m_api.PlayerGetPlayPos(m_playerHandle);   // Seek 内部做了帧对齐
                    UpdateProgressUI();
                }
            }
            return 0;

        case WM_PAINT: {                     // 绘制进度条 + 波形
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            DrawProgress(hdc);
            DrawWaveform(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:{
            KillTimer(hwnd, TIMER_PROGRESS);
            PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

/**
 * @brief 按钮分发(成员对象, 状态可跨点击保持)
 * @param iId 按钮 ID
 */
void CMainWindows::OnCommand(int iId){
    // dll 没加载成功时, 除"打开文件"(纯 UI 操作)外一律忽略
    if (iId != BTN_OPEN_FILE && !SdkReady())
        return;

    switch (iId){
    case BTN_RECORD_START_STOP:            // 开始 / 停止录音(同一按钮切换文字)
        AudioStartStopRec();
        break;
    case BTN_RECORD_PAUSE:                 // 暂停/继续录音
        AudioPauseResumeRec();
        break;
    case BTN_ENCRYPT:                      // 加密复选框(点击后勾选已自动翻转)
        if (!m_isRecording)
            m_api.RecorderSetAencEncrypt(m_recorderHandle);   // 翻转 recorder 内部加密状态
        // 让勾选显示与 recorder 真实状态保持一致
        SendMessageW(m_hChkEnc, BM_SETCHECK,
                     m_api.RecorderGetAencEncrypt(m_recorderHandle) ? BST_CHECKED : BST_UNCHECKED, 0);
        break;
    case BTN_OPEN_FILE:
        OpenFileDialog(m_hwnd);
        break;
    case BTN_START_STOP_PLAY:
        AudioStartStopPlay();
        break;
    case BTN_PLAY_PAUSE:
        AudioPauseResumePlay();
        break;
    }
}

// --------------录音相关------------------

/**
 * @brief 开始 / 停止录音(按当前状态)
 */
void CMainWindows::AudioStartStopRec(){
    if (!SdkReady()) return;
    if (!m_isRecording){
        // ---- 开始录音 ----
        // 先注册波形回调, 再开设备 —— 反过来的话第一块(100ms)的数据会漏掉。
        // 必须"每次开始录音都注册": SDK 在 StopRecording 里会清掉回调
        // (那时设备已静默, 清是安全的), 若只在窗口创建时注册一次,
        // 第二次录音起就再也收不到波形了。
        m_api.RecorderSetWaveCallback(m_recorderHandle, &CMainWindows::OnWaveFromAudio, this);

        // 波形从头开始: 丢掉上一轮的滚动窗口, 并把环形缓冲的读游标追到写游标
        // (否则新一轮录音会接着上一次的波形往后画)
        m_recWaveCount = 0;
        m_waveReadPos = m_waveWritePos.load(std::memory_order_acquire);

        // C 接口返回的是 int 状态码, 想按名字判断就转回枚举(AudioSdkState 序号两端一致)
        const int startSt = m_api.RecorderStart(m_recorderHandle);
        if (startSt != static_cast<int>(AudioSdk::AudioSdkState::NONE)){
            // 内存不足和"设备打不开"是两回事, 提示得分开 —— 否则用户会去查设备
            MessageBoxW(m_hwnd,
                        (startSt == static_cast<int>(AudioSdk::AudioSdkState::OUT_OF_MEMORY))
                            ? L"内存不足，无法开始录音"
                            : L"打开录音设备失败",
                        L"录音", MB_OK | MB_ICONERROR);
            return;
        }
        InvalidateWave();
        m_isRecording = true;
        m_recPaused   = false;
        SetWindowTextW(m_hBtnRec_Start_Stop, L"停止录音");
        SetWindowTextW(m_hBtnRecPause, L"暂停录音");
        EnableWindow(m_hBtnRecPause, TRUE);
        EnableWindow(m_hChkEnc, FALSE);          // 录制中不能改加密
        EnableWindow(m_hBtnOpen, FALSE);         // 录音时禁用播放区
        EnableWindow(m_hBtnPlay_Start_Stop, FALSE);
        EnableWindow(m_hBtnPlayPause, FALSE);
        UpdateRecTimeUI(0);                       // 录音时长归零
    }
    else{
        // ---- 停止录音 → 落盘 ----
        const int stopSt = m_api.RecorderStop(m_recorderHandle);
        m_isRecording = false;
        m_recPaused   = false;
        UpdateRecTimeUI(0);                         // 录音时长归零
        SetWindowTextW(m_hBtnRec_Start_Stop, L"开始录音");
        SetWindowTextW(m_hBtnRecPause, L"暂停录音");
        EnableWindow(m_hBtnRecPause, FALSE);
        EnableWindow(m_hChkEnc, TRUE);
        EnableWindow(m_hBtnOpen, TRUE);
        if (!m_curFile.empty())
            EnableWindow(m_hBtnPlay_Start_Stop, TRUE);

        if (stopSt == static_cast<int>(AudioSdk::AudioSdkState::OUT_OF_MEMORY)){
            MessageBoxW(m_hwnd, L"内存不足，录音数据不完整（已保存录到的部分）",
                        L"录音", MB_OK | MB_ICONWARNING);
            return;
        }
        if (stopSt != static_cast<int>(AudioSdk::AudioSdkState::NONE)){
            MessageBoxW(m_hwnd, L"录音保存失败", L"录音", MB_OK | MB_ICONERROR);
            return;
        }
        MessageBoxW(m_hwnd, m_api.RecorderGetAencEncrypt(m_recorderHandle)
                            ? L"录音已保存为加密 output.aenc"
                            : L"录音已保存为明文 output.wav",
                    L"录音", MB_OK | MB_ICONINFORMATION);
    }
}

/**
 * @brief 暂停/继续录音
 */
void CMainWindows::AudioPauseResumeRec(){
    if (!m_isRecording) return;
    m_api.RecorderPauseResume(m_recorderHandle);
    m_recPaused = m_api.RecorderGetIsPaused(m_recorderHandle) != 0;
    SetWindowTextW(m_hBtnRecPause, m_recPaused ? L"继续录音" : L"暂停录音");
}

/**
 * @brief 弹出"打开音频文件"对话框; 选中后把完整路径存入 m_curFile
 * @param hwndOwner 父窗口句柄(对话框模态于它)
 * @return 用户选定了文件返回 true(路径在 m_curFile); 取消返回 false
 */
bool CMainWindows::OpenFileDialog(HWND hwndOwner){
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize  = sizeof(ofn);                       // 结构体大小(必须填)
    ofn.hwndOwner    = hwndOwner;                         // 父窗口
    // 过滤: 两两一组(显示名\0 匹配串), 结尾要两个 \0
    ofn.lpstrFilter  = L"音频文件 (*.wav;*.aenc)\0*.wav;*.aenc\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile    = file;                              // 选中的路径写这里
    ofn.nMaxFile     = MAX_PATH;                          // 缓冲长度
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;  // 只许选已存在文件
    if (!GetOpenFileNameW(&ofn))                          // 用户取消 → false
        return false;

    m_curFile = file;                                     // 记住完整路径

    // 可选: 在窗口标题上显示当前文件, 直观反馈选到了什么。
    std::wstring title = L"Win32 音频播放器 - ";
    title += file;
    title += m_api.IsAencFile(WideToUtf8(file).c_str())
                 ? L"  (加密 .aenc)" : L"  (明文)";
    SetWindowTextW(m_hwnd, title.c_str());

    if (!m_isRecording && !m_isPlaying)
        EnableWindow(m_hBtnPlay_Start_Stop, TRUE);
    return true;
}

/**
 * @brief 更新录音时间文字
 * @param ms 录音时长(毫秒)
 */
void CMainWindows::UpdateRecTimeUI(DWORD ms){
    wchar_t buf[16], text[40];
    FormatMs(buf, std::size(buf), ms);                       // mm:ss.d
    swprintf_s(text, std::size(text), L"录音时长: %s", buf);
    SetWindowTextW(m_hLblRecTime, text);
}

// --------------播放相关------------------

/**
 * @brief 开始 / 停止播放(按当前状态)
 */
void CMainWindows::AudioStartStopPlay(){
    if (!SdkReady()) return;
    // ---- 停止播放 ----
    if (m_isPlaying) {
        m_api.PlayerStopPlay(m_playerHandle);
        m_isPlaying  = false;
        m_playPaused = false;
        m_playPosBytes = 0;
        m_dragging   = false;

        SetWindowTextW(m_hBtnPlay_Start_Stop, L"播放");
        SetWindowTextW(m_hBtnPlayPause, L"暂停");
        EnableWindow(m_hBtnPlay_Start_Stop,     TRUE);
        EnableWindow(m_hBtnPlayPause, FALSE);
        EnableWindow(m_hBtnOpen,      TRUE);
        EnableWindow(m_hBtnRec_Start_Stop, TRUE);
        EnableWindow(m_hChkEnc, TRUE);
        if (!m_recPaused)
            EnableWindow(m_hBtnRecPause, FALSE);

        UpdateProgressUI();
        return;
    };

    // ---- 开始播放 ----
    if (m_curFile.empty()){
        MessageBoxW(m_hwnd, L"请先点【打开文件】选择音频", L"播放", MB_OK | MB_ICONINFORMATION);
        return;
    }

    const int res = m_api.PlayerPlayFile(m_playerHandle, WideToUtf8(m_curFile).c_str());
    if (res != static_cast<int>(AudioSdk::AudioSdkState::NONE)){
        const wchar_t* msg = L"播放失败";
        switch (static_cast<AudioSdk::AudioSdkState>(res)){
            case AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED:msg = L"格式不支持"; break;
            case AudioSdk::AudioSdkState::FILE_OPEN_FAILED:msg = L"文件打开失败"; break;
            case AudioSdk::AudioSdkState::DEVICE_BUSY:msg = L"设备被占用"; break;
            case AudioSdk::AudioSdkState::DEVICE_NOT_FOUND:msg = L"找不到播放设备"; break;
            default: break;
        }
        MessageBoxW(m_hwnd, msg, L"播放", MB_OK | MB_ICONERROR);
        return;
    }

    // 播放已开始: 让 SDK 把整段波形算出来。
    // 这是同步回调(在 UI 线程内跑完), 回调直接把数据填进 m_fileWave, 无并发。
    m_fileWaveCount = 0;    // 先清空: 万一新文件算不出波形, 也别留着上一个文件的
    m_api.PlayerBuildWaveform(m_playerHandle, &CMainWindows::OnWaveFromFile, this);

    // 刷新波形区
    InvalidateWave();

    m_isPlaying  = true;
    m_playPaused = false;
    m_dragging   = false;
    m_playTotalBytes = m_api.PlayerGetTotalPos(m_playerHandle);
    m_playPosBytes   = 0;

    SetWindowTextW(m_hBtnPlay_Start_Stop, L"停止");
    SetWindowTextW(m_hBtnPlayPause, L"暂停");
    EnableWindow(m_hBtnPlay_Start_Stop, TRUE);
    EnableWindow(m_hBtnPlayPause, TRUE);
    EnableWindow(m_hBtnOpen, FALSE);
    EnableWindow(m_hBtnRec_Start_Stop, FALSE);
    EnableWindow(m_hBtnRecPause, FALSE);
    EnableWindow(m_hChkEnc, FALSE);

    UpdateProgressUI();
}

/**
 * @brief 暂停/继续播放
 */
void CMainWindows::AudioPauseResumePlay(){
    if (!m_isPlaying) return;
    if (m_playPaused){
        m_api.PlayerResumePlay(m_playerHandle);
        m_playPaused = false;
        SetWindowTextW(m_hBtnPlayPause, L"暂停");
    }
    else{
        m_api.PlayerPausePlay(m_playerHandle);
        m_playPaused = true;
        SetWindowTextW(m_hBtnPlayPause, L"继续");
    }
}

// --------------进度条相关------------------

/**
 * @brief 获取进度条在客户区的矩形(轨道位置)
 * @return RECT 进度条矩形
 */
RECT CMainWindows::ProgressRect() const{
    RECT rc = { 10, 82, 400, 98 };   // 左 10~400, 上 82~98(高 16 的一条轨道)
    return rc;
}

/**
 * @brief 鼠标客户区 x → 对应目标字节(并夹到 [0, total])
 * @param iClientX 鼠标客户区 x 坐标
 * @return DWORD 对应的目标字节位置
 */
DWORD CMainWindows::ClampToBytes(int iClientX){
    RECT rc = ProgressRect();
    if (iClientX < rc.left) iClientX = rc.left;
    if (iClientX > rc.right) iClientX = rc.right;
    const double k = (double)(iClientX - rc.left) / (rc.right - rc.left);
    return (DWORD)(k * m_playTotalBytes);
}

/**
 * @brief 刷新进度条
 */
void CMainWindows::InvalidateProgress(){
    RECT rc = ProgressRect();
    InvalidateRect(m_hwnd, &rc, FALSE);

    // 刷新波形区
    if (m_isPlaying)
        InvalidateWave();
}

/**
 * @brief 定时器: 问播放器播到哪 → 更新显示; 并检测"自然播完"复位
 */
void CMainWindows::OnTimerTick(){
    // 录音中: 已录字节 ×10 ÷ 每秒字节数 = 十分之一秒数
    if (!SdkReady()) return;                       // dll 没加载, 定时器空转就行
    if (m_isRecording){
        // 已录时长同样问 SDK 要毫秒, UI 不自己按采样率算
        UpdateRecTimeUI(m_api.RecorderGetRecordedMs(m_recorderHandle));
    }

    if (!m_isPlaying) return;                     // 没在播就不刷
    if (m_dragging) return;                        // 拖动预览中, 不抢位置

    m_playPosBytes   = m_api.PlayerGetPlayPos(m_playerHandle);
    m_playTotalBytes = m_api.PlayerGetTotalPos(m_playerHandle);

    UpdateProgressUI();

    // 自然播完: 之前还在播, 现在播放器自己停了(不是暂停)
    if (!m_playPaused && !m_api.PlayerIsPlaying(m_playerHandle)){
        m_playPosBytes = m_playTotalBytes;
        UpdateProgressUI();
        AudioStartStopPlay();
    }
}

/**
 * @brief 更新进度条显示
 */
void CMainWindows::UpdateProgressUI(){
    // 时间文字: 直接问 SDK 要毫秒(字节→时间的换算归 SDK, UI 不碰文件、不碰字节率)
    const DWORD posMs = m_api.PlayerGetPlayPosMs(m_playerHandle);
    const DWORD totMs = m_api.PlayerGetTotalPosMs(m_playerHandle);

    wchar_t now[16], tot[16], text[48];
    FormatMs(now, std::size(now), posMs);
    FormatMs(tot, std::size(tot), totMs);
    swprintf_s(text, std::size(text), L"%s / %s", now, tot);
    SetWindowTextW(m_hLblTime, text);

    InvalidateProgress();
}

/**
 * @brief 自绘进度条: 轨道底 + 已播蓝段 + 边框
 * @param hdc 设备上下文
 */
void CMainWindows::DrawProgress(HDC hdc){
    RECT rc = ProgressRect();

    double dRatio = 0.0;
    if (m_playTotalBytes > 0){
        dRatio = (double)m_playPosBytes / m_playTotalBytes;
        if (dRatio < 0) dRatio = 0;
        else if (dRatio > 1) dRatio = 1;
    }
    const int fillW = (int)((rc.right - rc.left) * dRatio);

    HBRUSH brTrack = CreateSolidBrush(RGB(225, 225, 225));   // 轨道底(浅灰)
    FillRect(hdc, &rc, brTrack);
    DeleteObject(brTrack);

    if (fillW > 0){                                          // 已播段(蓝)
        RECT rcFill = { rc.left, rc.top, rc.left + fillW, rc.bottom };
        HBRUSH brFill = CreateSolidBrush(RGB(0, 120, 215));
        FillRect(hdc, &rcFill, brFill);
        DeleteObject(brFill);
    }

    FrameRect(hdc, &rc, (HBRUSH)GetStockObject(GRAY_BRUSH));  // 边框
}

// --------------波形相关------------------

/**
 * @brief 波形区在客户区的矩形
 * @return RECT 波形区矩形(在进度条下方)
 */
RECT CMainWindows::WaveRect() const{
    RECT rc = { 10, 110, 590, 360 };
    return rc;
}

/**
 * @brief 让波形区重绘
 */
void CMainWindows::InvalidateWave(){
    RECT rc = WaveRect();
    InvalidateRect(m_hwnd, &rc, FALSE);
}

/**
 * @brief 自绘波形: 背景 + 中轴线 + 波形(上下对称) + 播放位置竖线
 * @param hdc 设备上下文
 * @note 数据源按当前状态选: 录音中画实时滚动波形, 播放中画整段文件波形。
 *       点数多于像素列时, 按列再合并一次(取该列覆盖范围的 min/max) —— 这样
 *       不管缓冲里有多少点, 都能完整落到画布上, 不会丢峰。
 */
void CMainWindows::DrawWaveform(HDC hdc){
    const RECT rc = WaveRect();

    // 背景
    HBRUSH brBg = CreateSolidBrush(RGB(250, 250, 250));
    FillRect(hdc, &rc, brBg);
    DeleteObject(brBg);

    const int midY  = (rc.top + rc.bottom) / 2;
    const int halfH = (rc.bottom - rc.top) / 2 - 2;   // 留 2px 边距, 满幅时也不贴边

    // 中轴线(零电平)
    HPEN penAxis = CreatePen(PS_SOLID, 1, RGB(210, 210, 210));
    HPEN penOld  = (HPEN)SelectObject(hdc, penAxis);
    MoveToEx(hdc, rc.left, midY, NULL);
    LineTo(hdc, rc.right, midY);
    SelectObject(hdc, penOld);
    DeleteObject(penAxis);

    // 选数据源: 录音中看实时输入, 否则看已打开文件的整段波形
    // (停止播放后仍然保留文件波形, 方便回看; 只是不再画播放位置竖线)
    const float (*data)[2] = nullptr;
    int count = 0;
    if (m_isRecording){
        data  = m_recWave;
        count = m_recWaveCount;
    }
    else {
        data  = m_fileWave;
        count = m_fileWaveCount;
    }

    if (data && count > 0){
        const int w = rc.right - rc.left;

        // 每个像素列对应缓冲里的一段 [i0, i1): 把这段的 min/max 合起来再画
        HPEN penWave = CreatePen(PS_SOLID, 1, RGB(0, 120, 215));
        penOld = (HPEN)SelectObject(hdc, penWave);

        for (int x = 0; x < w; ++x){
            int i0 = (int)((long long)x * count / w);
            int i1 = (int)((long long)(x + 1) * count / w);
            if (i1 <= i0) i1 = i0 + 1;                 // 点数少于列数时至少取一个
            if (i0 >= count) break;

            float mn = data[i0][0];
            float mx = data[i0][1];
            for (int i = i0 + 1; i < i1 && i < count; ++i){
                if (data[i][0] < mn) mn = data[i][0];
                if (data[i][1] > mx) mx = data[i][1];
            }

            // 归一化的 [-1,1] → 屏幕 y。max 在上, min 在下。
            int yTop = midY - (int)(mx * halfH);
            int yBot = midY - (int)(mn * halfH);
            if (yTop == yBot) yBot = yTop + 1;         // 静音时给 1px 高度, 不然画不出来
            if (yTop < rc.top)    yTop = rc.top;
            if (yBot > rc.bottom) yBot = rc.bottom;

            const int px = rc.left + x;
            MoveToEx(hdc, px, yTop, NULL);
            LineTo(hdc, px, yBot);
        }

        SelectObject(hdc, penOld);
        DeleteObject(penWave);

        // 播放中: 在波形上叠一条当前位置的竖线
        if (m_isPlaying && m_playTotalBytes > 0){
            int px = rc.left + (int)((double)m_playPosBytes / m_playTotalBytes * w);
            if (px < rc.left) px = rc.left;
            if (px > rc.right - 1) px = rc.right - 1;

            HPEN penCur = CreatePen(PS_SOLID, 2, RGB(232, 17, 35));
            penOld = (HPEN)SelectObject(hdc, penCur);
            MoveToEx(hdc, px, rc.top, NULL);
            LineTo(hdc, px, rc.bottom);
            SelectObject(hdc, penOld);
            DeleteObject(penCur);
        }

        // 缩放标注: 录音看的是"最近一小段"(滚动窗口), 播放看的是"整个文件",
        // 两者时间跨度差很多, 波形看起来自然不一样。不标出来容易让人以为
        // "同一个文件怎么长得不同" —— 其实只是缩放级别不同, 数据没差别。
        wchar_t span[16], hint[64];
        if (m_isRecording){
            // 窗口还没填满时, 显示的就是已经攒到的那一段;
            // 填满之后固定为整个窗口长度(之后就是滚动, 不再变长)
            const int shown = m_recWaveCount < REC_WAVE_POINTS ? m_recWaveCount : REC_WAVE_POINTS;
            // 每块 100ms 产出 AUDIO_SDK_WAVE_BLOCK_POINTS 个点 → 每点 100/256 ms
            FormatMs(span, std::size(span), (DWORD)(shown * 100 / AUDIO_SDK_WAVE_BLOCK_POINTS));
            swprintf_s(hint, std::size(hint), L"录音中 · 显示最近 %s", span);
        }
        else{
            FormatMs(span, std::size(span), m_api.PlayerGetTotalPosMs(m_playerHandle));
            swprintf_s(hint, std::size(hint), L"全长 %s", span);
        }
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(130, 130, 130));
        RECT rcHint = { rc.left + 6, rc.top + 4, rc.right - 6, rc.top + 22 };
        DrawTextW(hdc, hint, -1, &rcHint, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }
    else{
        // 没数据时给个提示文字, 免得空白让人以为坏了
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(160, 160, 160));
        RECT rcText = rc;
        DrawTextW(hdc, L"录音或播放时显示波形", -1, &rcText,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    FrameRect(hdc, &rc, (HBRUSH)GetStockObject(GRAY_BRUSH));   // 边框
}

/**
 * @brief 检查 dll 是否加载成功
 * @return true 已加载
 * @return false 未加载
 */
bool CMainWindows::SdkReady() const{
    return m_api.hModule != nullptr;
}
