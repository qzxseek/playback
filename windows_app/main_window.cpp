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

// --------------双链路工作线程--------------

/**
 * @brief WM_CREATE: 起全部工作线程(两条录音 lane + 一条播放线程)
 */
void CMainWindows::StartWorkers(){
    for (int i = 0; i < 2; ++i)
        m_recLanes[i].worker = std::thread(RecWorkerLoop, this, i);
    m_playWorker = std::thread(PlayWorkerLoop, this);
}

/**
 * @brief 析构/收尾: 两条录音 lane 和播放线程各投一个 Exit → 逐一 join
 * @note 全程序唯一允许 UI 等线程的地方(关程序)。先录音后播放:
 *       录音 lane 可能还卡在落盘, 先给它机会写完
 */
void CMainWindows::StopWorkers(){
    for (int i = 0; i < 2; ++i)
        m_recLanes[i].q.Push({Cmd::Exit, nullptr});    
    m_playQ.Push({Cmd::Exit, L""});
    for (int i = 0; i < 2; ++i)
        if (m_recLanes[i].worker.joinable())
            m_recLanes[i].worker.join();
    if (m_playWorker.joinable())
        m_playWorker.join();
}

/**
 * @brief 录音 lane 主循环: 串行执行本 lane 收到的 Start/Stop 命令
 * @param self 实例指针(静态入口传 this)
 * @param lane 本线程对应的 lane 号(0/1)
 * @note 一个会话(新建对象→Start→…→Stop落盘)全部在本线程完成 ——
 *       这就是"录音器对象的 m_vecRecData 不被两条会话共用"的结构保证。
 *       两条 lane 互不同步, lane0 落盘时 lane1 照常开录
 */
void CMainWindows::RecWorkerLoop(CMainWindows* self, int lane){
    auto& q = self->m_recLanes[lane].q;
    for (;;){
        Cmd::RecCommand cmd;
        if (!q.Pop(cmd))
            break;                                    // Exit 命令
        // 同一会话的 Start/Stop 都落在本线程本对象上 —— 串行, 无并发
        const int st = (cmd.op == Cmd::RecStart)
            ? self->m_recApi.RecorderStart(cmd.handle)
            : self->m_recApi.RecorderStop(cmd.handle);
        ::PostMessage(self->m_hwnd, WM_APP_SDK_DONE,
                      MAKEWPARAM(cmd.op, lane), static_cast<LPARAM>(st));
    }
}

/**
 * @brief 播放链主循环: PlayFile + BuildWaveform 都在本线程跑完再回报
 * @param self 实例指针
 */
void CMainWindows::PlayWorkerLoop(CMainWindows* self){
    for (;;){
        Cmd::PlayCommand cmd;
        if (!self->m_playQ.Pop(cmd))
            break;                                    // Exit 命令
        if (cmd.op != Cmd::PlayFile) continue;

        const std::string utf8Path = WideToUtf8(cmd.path);
        const int st = self->m_playApi.PlayerPlayFile(self->m_playerHandle, utf8Path.c_str());
        if (st == static_cast<int>(AudioSdk::AudioSdkState::NONE)){
            // 整段波形: 回调跑在本线程, 数据经锁拷进成员(UI 画的时候再拷出)
            self->CopyFileWave(nullptr, 0);           // 先清掉上一个文件的波形
            self->m_playApi.PlayerBuildWaveform(self->m_playerHandle,
                                                &CMainWindows::OnWaveFromFile, self);
        }
        ::PostMessage(self->m_hwnd, WM_APP_SDK_DONE,
                      MAKEWPARAM(Cmd::PlayFile, 0), static_cast<LPARAM>(st));
    }
}

CMainWindows::CMainWindows(){
    // 显式加载: 两条链各开一次库
    if (!m_recApi.Load()){
        MessageBoxW(nullptr, Utf8ToWide(m_recApi.LastError()).c_str(),
                    L"加载 audio_sdk.dll 失败", MB_OK | MB_ICONERROR);
        m_playApi.Unload();           
        return;                       // 句柄保持 nullptr; CreateControls 里会把音频按钮禁掉
    }
    if (!m_playApi.Load()){
        MessageBoxW(nullptr, Utf8ToWide(m_playApi.LastError()).c_str(),
                    L"加载 audio_sdk.dll 失败", MB_OK | MB_ICONERROR);
        m_recApi.Unload();
        return;
    }

    // 播放链全程只有一个对象, 起来时建好
    m_playerHandle = m_playApi.PlayerCreate();
    m_playerHandles.push_back(m_playerHandle);
    // 录音器不预建: 每次开始录音时按需新建(独立 m_vecRecData),
    m_recorderHandle = nullptr;

    if (!m_playerHandle)
        MessageBoxW(nullptr, L"创建播放对象失败(内存不足?)",
                    L"音频 SDK", MB_OK | MB_ICONERROR);
}

CMainWindows::~CMainWindows(){
    if (m_isRecording)
        AudioStartStopRec();
    if (m_isPlaying)
        AudioStartStopPlay();

    // 关闭录音和播放两条工作线程
    StopWorkers();

    // 先销毁 dll 里的所有对象, 再卸 dll —— 顺序反了就是往已卸载的代码里跳
    for (void* h : m_recorderHandles)
        m_recApi.RecorderDestroy(h);
    m_recorderHandles.clear();
    for (void* h : m_playerHandles)
        m_playApi.PlayerDestroy(h);
    m_playerHandles.clear();
    m_recorderHandle = nullptr;
    m_playerHandle   = nullptr;

    // 释放录音和播放链的库句柄
    m_recApi.Unload();
    m_playApi.Unload();
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
 * @brief 文件波形回调 —— 在【播放线程】执行
 * @param minmax 峰值对
 * @param points 点数
 * @param userData 注册时传进来的 this
 */
void CMainWindows::OnWaveFromFile(const float* minmax, int points, void* userData){
    auto* self = static_cast<CMainWindows*>(userData);
    if (!self) return;
    self->CopyFileWave(minmax, points);
}

/**
 * @brief 播放线程写一整批文件波形(锁内); minmax 为空表示清空
 */
void CMainWindows::CopyFileWave(const float* minmax, int points){
    std::lock_guard<std::mutex> lk(m_fileWaveMtx);
    if (!minmax || points <= 0){
        m_fileWaveCount = 0;
        return;
    }
    const int n = points < AUDIO_SDK_WAVE_FILE_POINTS ? points : AUDIO_SDK_WAVE_FILE_POINTS;
    for (int i = 0; i < n; ++i){
        m_fileWave[i][0] = minmax[i * 2];
        m_fileWave[i][1] = minmax[i * 2 + 1];
    }
    m_fileWaveCount = n;
}

/**
 * @brief UI 线程拷一份文件波形快照(锁内), 拿去画, 不和播放线程的写入纠缠
 * @return 有效点数(out 里前这么多对有效)
 */
int CMainWindows::SnapshotFileWave(float (&out)[AUDIO_SDK_WAVE_FILE_POINTS][2]) const{
    std::lock_guard<std::mutex> lk(m_fileWaveMtx);
    if (m_fileWaveCount > 0)
        std::memcpy(out, m_fileWave, sizeof(float) * m_fileWaveCount * 2);
    return m_fileWaveCount;
}

/**
 * @brief 把一批峰值点接到滚动窗口末尾, 窗口装不下就从最老的开始挤出去
 * @param minmax 峰值对 [min0,max0,min1,max1,...](布局同回调参数)
 * @param points 点数
 * @note 只有 UI 线程调用, 所以 m_recWave / m_recWaveCount 不需要同步
 */
void CMainWindows::AppendToRecWave(const float* minmax, int points){
    if (!minmax || points <= 0) return;

    // 一次来的点比整个窗口还多: 只留最后 REC_WAVE_POINTS 个
    if (points >= REC_WAVE_POINTS){
        std::memcpy(m_recWave, minmax + (points - REC_WAVE_POINTS) * 2,
                    sizeof(float) * REC_WAVE_POINTS * 2);
        m_recWaveCount = REC_WAVE_POINTS;
        return;
    }

    // 窗口放不下的老点先丢掉: 一次 memmove 把保留的部分挪到开头。
    // 这样比"每来一个点就挪一格"省掉大量重复拷贝。
    if (m_recWaveCount + points > REC_WAVE_POINTS){
        const int drop = m_recWaveCount + points - REC_WAVE_POINTS;
        const int iKeep = m_recWaveCount - drop;      // drop <= m_recWaveCount, 恒非负
        if (iKeep > 0)
            std::memmove(m_recWave, m_recWave + drop,
                         sizeof(m_recWave[0]) * static_cast<size_t>(iKeep));
        m_recWaveCount = iKeep;
    }

    // 新点接到末尾
    std::memcpy(m_recWave[m_recWaveCount], minmax, sizeof(float) * points * 2);
    m_recWaveCount += points;
}

/**
 * @brief 把 SDK 环里攒着的波形点全取出来, 接进 UI 侧的滚动窗口(拉模式)
 * @return 本次是否取到了新点
 * @note 只有 UI 线程调用(定时器里)。
 *       循环取到 0 为止: SDK 那边可能攒了不止一块(UI 卡一下就会这样),
 *       一次只拉一块会让显示越落越远, 所以每次拉干。
 */
bool CMainWindows::ConsumeWaveRing(){
    if (!SdkReady() || !m_recorderHandle) return false;
    bool got = false;
    for (;;){
        const int n = m_recApi.RecorderReadWave(m_recorderHandle, m_wavePull,
                                                AUDIO_SDK_WAVE_BLOCK_POINTS);
        if (n <= 0) break;              // 0 = 暂无新数据, 不是错误
        AppendToRecWave(m_wavePull, n);
        got = true;
    }
    return got;
}

/**
 * @brief 每次开录新建一个录音器对象(会话专属), 挂进容器统一销毁
 * @param outPath 回填本会话的输出路径(不含扩展名), 供结果提示显示真实文件名
 * @return 新对象句柄; 创建失败返回 nullptr
 */
void* CMainWindows::TakeRecorder(std::wstring& outPath){
    void* h = m_recApi.RecorderCreate();
    if (h){
        outPath = NextOutputPath();
        m_recorderHandles.push_back(h);
        m_recApi.RecorderSetOutputPath(h, WideToUtf8(outPath).c_str());
        // 新建对象的加密开关要跟 UI 勾选状态对齐(SDK 默认开)
        if (!m_encryptOn)
            m_recApi.RecorderSetAencEncrypt(h);
    }
    return h;
}

/**
 * @brief 每会话输出路径: exe 目录\output_时间戳_序号(.aenc/.wav 由 SDK 按开关定)
 */
std::wstring CMainWindows::NextOutputPath(){
    wchar_t exePath[MAX_PATH] = {};
    std::wstring base;
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0){
        base = exePath;
        const size_t slash = base.find_last_of(L'\\');
        base.resize(slash == std::wstring::npos ? 0 : slash + 1);   // 留下目录
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[48];
    swprintf_s(stamp, std::size(stamp), L"output_%04u%02u%02u_%02u%02u%02u_%u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               ++m_outputSeq);
    return base + stamp;
}

/**
 * @brief WM_CREATE: 记录主窗口句柄并创建全部控件
 * @param hwnd 主窗口句柄
 */
void CMainWindows::OnCreate(HWND hwnd){
    m_hwnd = hwnd;
    CreateControls(hwnd);
    StartWorkers();                                            
    SetTimer(hwnd, TIMER_PROGRESS, TIMER_INTERVAL_MS, NULL);   // 启动进度心跳
}

/**
 * @brief 创建全部子控件
 * @param hwnd 主窗口句柄
 */
void CMainWindows::CreateControls(HWND hwnd){
    HINSTANCE hInst = GetModuleHandle(NULL);

    // 录音区(坐标全部来自 Layout, 见 main_window.h)
    m_hBtnRec_Start_Stop = CreateWindowEx(0, L"BUTTON", L"开始录音",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        Layout::kRecBtnX, Layout::kRecRowTop, Layout::kBtnW, Layout::kRowHeight,
        hwnd, (HMENU)(INT_PTR)BTN_RECORD_START_STOP, hInst, NULL);
    m_hBtnRecPause = CreateWindowEx(0, L"BUTTON", L"暂停录音",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        Layout::kRecPauseX, Layout::kRecRowTop, Layout::kBtnW, Layout::kRowHeight,
        hwnd, (HMENU)(INT_PTR)BTN_RECORD_PAUSE, hInst, NULL);
    m_hChkEnc = CreateWindowEx(0, L"BUTTON", L"加密",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        Layout::kEncX, Layout::kRecRowTop, Layout::kSmallBtnW, Layout::kRowHeight,
        hwnd, (HMENU)(INT_PTR)BTN_ENCRYPT, hInst, NULL);
    m_hLblRecTime = CreateWindowEx(0, L"STATIC", L"录音时长: 00:00.0",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        Layout::kTextColX, Layout::kRecRowTop + Layout::kLabelInset,
        Layout::kTextColW, Layout::kLabelH,
        hwnd, (HMENU)(INT_PTR)IDC_LBL_REC_TIME, hInst, NULL);

    // 播放区
    m_hBtnOpen = CreateWindowEx(0, L"BUTTON", L"打开文件",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        Layout::kOpenX, Layout::kPlayRowTop, Layout::kBtnW, Layout::kRowHeight,
        hwnd, (HMENU)(INT_PTR)BTN_OPEN_FILE, hInst, NULL);
    m_hBtnPlay_Start_Stop = CreateWindowEx(0, L"BUTTON", L"播放",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        Layout::kPlayBtnX, Layout::kPlayRowTop, Layout::kSmallBtnW, Layout::kRowHeight,
        hwnd, (HMENU)(INT_PTR)BTN_START_STOP_PLAY, hInst, NULL);
    m_hBtnPlayPause = CreateWindowEx(0, L"BUTTON", L"暂停",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        Layout::kPlayPauseX, Layout::kPlayRowTop, Layout::kPlayPauseW, Layout::kRowHeight,
        hwnd, (HMENU)(INT_PTR)BTN_PLAY_PAUSE, hInst, NULL);

    // 时间/状态文字(与录音时长同一列, 右对齐到公共右边界)
    m_hLblTime = CreateWindowEx(0, L"STATIC", L"00:00.0 / 00:00.0",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        Layout::kTextColX, Layout::kPlayRowTop + Layout::kLabelInset,
        Layout::kTextColW, Layout::kLabelH,
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
    // 勾选框初始状态: UI 侧记录的加密开关(与每会话新建对象时套用的值一致, 默认开)
    SendMessageW(m_hChkEnc, BM_SETCHECK,
                 m_encryptOn ? BST_CHECKED : BST_UNCHECKED, 0);
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

        case WM_APP_SDK_DONE:                // 工作线程回报: 命令码 + 录音 lane(wParam) + 状态码(lParam)
            OnSdkDone(LOWORD(wParam), HIWORD(wParam), static_cast<int>(lParam));
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
                    m_playApi.PlayerSeek(m_playerHandle, m_playPosBytes);          
                    m_playPosBytes = m_playApi.PlayerGetPlayPos(m_playerHandle);   
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
        // 开关记在 UI 侧: 录音器每会话新建, 建对象时套用当下勾选值
        m_encryptOn = !m_encryptOn;
        SendMessageW(m_hChkEnc, BM_SETCHECK,
                     m_encryptOn ? BST_CHECKED : BST_UNCHECKED, 0);
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
        std::wstring sOutPath;
        void* handle = TakeRecorder(sOutPath);
        if (!handle){
            MessageBoxW(m_hwnd, L"创建录音器失败", L"录音", MB_OK | MB_ICONERROR);
            return;
        }

        m_recWaveCount = 0;
        m_activeLane = m_recLaneIdx;
        m_recorderHandle = handle;                      // 轮询(计时/波形)用当前会话对象
        m_laneRec[m_activeLane]  = handle;              
        m_lanePath[m_activeLane] = sOutPath;        
        m_recBusy = true;                          // 设备打开中, 结果回来前开始按钮置灰
        SetRecGroupEnabled(false);
        m_recLanes[m_activeLane].q.Push({Cmd::RecStart, handle});
    }
    else{
        // ---- 停止录音 ----
        if (ConsumeWaveRing()) InvalidateWave();

        const int lane = m_activeLane;
        void* handle = m_laneRec[lane];                 // 本会话对象(Start 时记下的)
        m_laneRec[lane] = nullptr;

        m_isRecording  = false;
        m_recPaused    = false;
        m_savingHandle[lane] = handle;                  // 落盘结果回来时按 lane 终读它的波形尾巴
        UpdateRecTimeUI(0);                         // 录音时长归零
        SetWindowTextW(m_hBtnRec_Start_Stop, L"开始录音");
        SetWindowTextW(m_hBtnRecPause, L"暂停录音");
        EnableWindow(m_hBtnRecPause, FALSE);
        EnableWindow(m_hChkEnc, TRUE);
        EnableWindow(m_hBtnOpen, TRUE);
        if (!m_curFile.empty() && !m_playBusy)
            EnableWindow(m_hBtnPlay_Start_Stop, TRUE);
        // 开始按钮保持可用 —— 立刻能开下一段, 不等这段写完盘(乒乓的意义)
        SetWindowTextW(m_hLblRecTime, L"正在保存…");
        m_recLanes[lane].q.Push({Cmd::RecStop, handle});
    }
    m_recLaneIdx ^= 1;                             // 乒乓翻到另一条 lane(下轮用)
}

/**
 * @brief 工作线程结果处理: 按命令码分岔, 更新 UI 状态并恢复按钮
 * @param op 命令码(LOWORD(wParam))
 * @param lane 发来结果的录音 lane 号(HIWORD(wParam); 播放链恒 0)
 * @param state AudioSdkState 状态码(lParam)
 */
void CMainWindows::OnSdkDone(UINT op, UINT lane, int state){
    if (op == Cmd::RecStart){
        m_recBusy = false;
        if (state != static_cast<int>(AudioSdk::AudioSdkState::NONE)){
            // 内存不足和"设备打不开"是两回事, 提示得分开 —— 否则用户会去查设备
            MessageBoxW(m_hwnd,
                        (state == static_cast<int>(AudioSdk::AudioSdkState::OUT_OF_MEMORY))
                            ? L"内存不足，无法开始录音"
                            : L"打开录音设备失败",
                        L"录音", MB_OK | MB_ICONERROR);
            m_isRecording = false;
            m_recorderHandle = nullptr;            // 会话作废(对象仍由析构统一销毁)
        }
        else{
            m_isRecording = true;
            m_recPaused   = false;
        }
        SetRecGroupEnabled(true);
        if (m_isRecording){
            SetWindowTextW(m_hBtnRec_Start_Stop, L"停止录音");
            EnableWindow(m_hBtnRecPause, TRUE);
            EnableWindow(m_hChkEnc, FALSE);          // 录制中不能改加密
            EnableWindow(m_hBtnOpen, FALSE);         // 录音时禁用播放区
            EnableWindow(m_hBtnPlay_Start_Stop, FALSE);
            EnableWindow(m_hBtnPlayPause, FALSE);
            UpdateRecTimeUI(0);                     // 录音时长归零
        }
        return;
    }

    if (op == Cmd::RecStop){
        // 设备已停、数据已写盘。波形尾巴归本会话对象所有, 从它那里终读最后一次。
        // 按 lane 取句柄: 另一条 lane 可能也在落盘, 各认各的会话对象。
        if (m_savingHandle[lane]){
            float tail[AUDIO_SDK_WAVE_BLOCK_POINTS * 2];
            for (;;){
                const int n = m_recApi.RecorderReadWave(m_savingHandle[lane], tail,
                                                        AUDIO_SDK_WAVE_BLOCK_POINTS);
                if (n <= 0) break;
                AppendToRecWave(tail, n);
            }
            InvalidateWave();
        }
        m_savingHandle[lane] = nullptr;

        // 提示里带上真实文件名: 每段录音都是独立文件, 不报名字用户找不到刚存的那个
        const std::wstring& full = m_lanePath[lane];
        const size_t sep = full.find_last_of(L"\\/");
        const std::wstring name = (sep == std::wstring::npos) ? full : full.substr(sep + 1);
        wchar_t msg[128];
        if (state == static_cast<int>(AudioSdk::AudioSdkState::OUT_OF_MEMORY))
            MessageBoxW(m_hwnd, L"内存不足，录音数据不完整（已保存录到的部分）",
                        L"录音", MB_OK | MB_ICONWARNING);
        else if (state != static_cast<int>(AudioSdk::AudioSdkState::NONE))
            MessageBoxW(m_hwnd, L"录音保存失败", L"录音", MB_OK | MB_ICONERROR);
        else{
            swprintf_s(msg, std::size(msg), L"录音已保存为 %s%s",
                       name.c_str(), m_encryptOn ? L".aenc" : L".wav");
            MessageBoxW(m_hwnd, msg, L"录音", MB_OK | MB_ICONINFORMATION);
        }
        m_lanePath[lane].clear();
        SetWindowTextW(m_hLblRecTime, L"录音时长: 00:00.0");
        return;
    }

    if (op == Cmd::PlayFile){
        m_playBusy = false;
        if (state != static_cast<int>(AudioSdk::AudioSdkState::NONE)){
            // 开始播放失败: 复位播放区, 提示失败原因
            m_isPlaying = false;
            m_playPaused = false;
            SetPlayGroupIdle();
            ShowPlayError(state);
            return;
        }
        // 播放已开始: 刷波形、记总长、播放区进入"播放中"布局
        InvalidateWave();
        m_isPlaying  = true;
        m_playPaused = false;
        m_dragging   = false;
        m_playTotalBytes = m_playApi.PlayerGetTotalPos(m_playerHandle);
        m_playPosBytes   = 0;
        SetPlayGroupPlaying();
        UpdateProgressUI();
    }
}

/**
 * @brief 暂停/继续录音
 */
void CMainWindows::AudioPauseResumeRec(){
    if (!m_isRecording) return;
    m_recApi.RecorderPauseResume(m_recorderHandle);
    m_recPaused = m_recApi.RecorderGetIsPaused(m_recorderHandle) != 0;
    SetWindowTextW(m_hBtnRecPause, m_recPaused ? L"继续录音" : L"暂停录音");
}

/**
 * @brief 录音组按钮统一置灰/恢复(开始/停止/暂停/加密 + 关联的会话对象操作)
 * @param on true=恢复可用, false=置灰
 */
void CMainWindows::SetRecGroupEnabled(bool on){
    EnableWindow(m_hBtnRec_Start_Stop, on);
    EnableWindow(m_hBtnRecPause, on && m_isRecording);   // 暂停只在录音中可用
    EnableWindow(m_hChkEnc, on && !m_isRecording);       // 加密只在非录制中可改
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
    title += m_recApi.IsAencFile(WideToUtf8(file).c_str())
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
 * @brief 按状态码给播放失败文案(开始/停止共用)
 * @param state AudioSdkState 状态码
 */
void CMainWindows::ShowPlayError(int state){
    const wchar_t* msg = L"播放失败";
    switch (static_cast<AudioSdk::AudioSdkState>(state)){
        case AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED:msg = L"格式不支持"; break;
        case AudioSdk::AudioSdkState::FILE_OPEN_FAILED:msg = L"文件打开失败"; break;
        case AudioSdk::AudioSdkState::DEVICE_BUSY:msg = L"设备被占用"; break;
        case AudioSdk::AudioSdkState::DEVICE_NOT_FOUND:msg = L"找不到播放设备"; break;
        default: break;
    }
    MessageBoxW(m_hwnd, msg, L"播放", MB_OK | MB_ICONERROR);
}

/**
 * @brief 播放组按钮按"播放中"布局刷新(停止/暂停可用, 打开文件与录音组禁用)
 */
void CMainWindows::SetPlayGroupPlaying(){
    SetWindowTextW(m_hBtnPlay_Start_Stop, L"停止");
    SetWindowTextW(m_hBtnPlayPause, L"暂停");
    EnableWindow(m_hBtnPlay_Start_Stop, TRUE);
    EnableWindow(m_hBtnPlayPause, TRUE);
    EnableWindow(m_hBtnOpen, FALSE);
    EnableWindow(m_hBtnRec_Start_Stop, FALSE);
    EnableWindow(m_hBtnRecPause, FALSE);
    EnableWindow(m_hChkEnc, FALSE);
}

/**
 * @brief 播放组按钮按"非播放中"布局刷新(同时尊重录音状态与忙标志)
 */
void CMainWindows::SetPlayGroupIdle(){
    SetWindowTextW(m_hBtnPlay_Start_Stop, L"播放");
    SetWindowTextW(m_hBtnPlayPause, L"暂停");
    EnableWindow(m_hBtnPlay_Start_Stop, !m_curFile.empty() && !m_playBusy);
    EnableWindow(m_hBtnPlayPause, FALSE);
    EnableWindow(m_hBtnOpen,      TRUE);
    EnableWindow(m_hBtnRec_Start_Stop, !m_recBusy && !m_isRecording);
    EnableWindow(m_hChkEnc, !m_isRecording);
    EnableWindow(m_hBtnRecPause, m_isRecording);       // 录音中暂停仍可用
}

/**
 * @brief 开始 / 停止播放(按当前状态)。
 *        开始 = 投 PlayFile 给播放线程(读文件+校验, 慢); 停止 = UI 线程同步停(快)
 */
void CMainWindows::AudioStartStopPlay(){
    if (!SdkReady()) return;
    // ---- 停止播放 ----
    if (m_isPlaying) {
        m_playApi.PlayerStopPlay(m_playerHandle);
        m_isPlaying  = false;
        m_playPaused = false;
        m_playPosBytes = 0;
        m_dragging   = false;

        SetPlayGroupIdle();
        UpdateProgressUI();
        return;
    };

    // ---- 开始播放: 投命令给播放线程, 置灰等结果 ----
    if (m_curFile.empty()){
        MessageBoxW(m_hwnd, L"请先点【打开文件】选择音频", L"播放", MB_OK | MB_ICONINFORMATION);
        return;
    }

    m_playBusy = true;
    SetPlayGroupEnabled(FALSE);
    SetWindowTextW(m_hLblTime, L"正在打开…");
    m_playQ.Push({Cmd::PlayFile, m_curFile});           // 路径拷贝进命令, UI 立即返回
}

/**
 * @brief 播放组按钮统一置灰/恢复(开始播放/暂停/打开文件)
 */
void CMainWindows::SetPlayGroupEnabled(bool on){
    EnableWindow(m_hBtnPlay_Start_Stop, on);
    EnableWindow(m_hBtnPlayPause, on && m_isPlaying);
    EnableWindow(m_hBtnOpen, on);
}

/**
 * @brief 暂停/继续播放
 */
void CMainWindows::AudioPauseResumePlay(){
    if (!m_isPlaying) return;
    if (m_playPaused){
        m_playApi.PlayerResumePlay(m_playerHandle);
        m_playPaused = false;
        SetWindowTextW(m_hBtnPlayPause, L"暂停");
    }
    else{
        m_playApi.PlayerPausePlay(m_playerHandle);
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
    // 左右都对齐到公共边界: 右边界和波形区/文字列是同一条竖线
    RECT rc = { Layout::kMargin, Layout::kProgTop,
                Layout::kRight,  Layout::kProgTop + Layout::kProgHeight };
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
        UpdateRecTimeUI(m_recApi.RecorderGetRecordedMs(m_recorderHandle));
        // 波形: 音频线程只往 SDK 自己的环里写, 这里(UI 线程)每 100ms 拉一次
        if (ConsumeWaveRing()) InvalidateWave();
    }

    if (!m_isPlaying) return;                     // 没在播就不刷
    if (m_dragging) return;                        // 拖动预览中, 不抢位置

    m_playPosBytes   = m_playApi.PlayerGetPlayPos(m_playerHandle);
    m_playTotalBytes = m_playApi.PlayerGetTotalPos(m_playerHandle);

    UpdateProgressUI();

    // 自然播完: 之前还在播, 现在播放器自己停了(不是暂停)
    if (!m_playPaused && !m_playApi.PlayerIsPlaying(m_playerHandle)){
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
    const DWORD posMs = m_playApi.PlayerGetPlayPosMs(m_playerHandle);
    const DWORD totMs = m_playApi.PlayerGetTotalPosMs(m_playerHandle);

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
    RECT rc = { Layout::kMargin, Layout::kWaveTop,
                Layout::kRight,  Layout::kWaveTop + Layout::kWaveHeight };
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
    // (停止播放后仍然保留文件波形, 方便回看; 只是不再画播放位置竖线)。
    // 文件波形在播放线程里被写, 这里拷一份快照再画, 不跟写入纠缠。
    const float (*data)[2] = nullptr;
    int count = 0;
    if (m_isRecording){
        data  = m_recWave;
        count = m_recWaveCount;
    }
    else {
        count = SnapshotFileWave(m_fileWaveSnap);
        data  = m_fileWaveSnap;
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
            FormatMs(span, std::size(span), (DWORD)(shown * AUDIO_SDK_BLOCK_MS / AUDIO_SDK_WAVE_BLOCK_POINTS));
            swprintf_s(hint, std::size(hint), L"录音中 · 显示最近 %s", span);
        }
        else{
            FormatMs(span, std::size(span), m_playApi.PlayerGetTotalPosMs(m_playerHandle));
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
    return m_recApi.hModule != nullptr && m_playApi.hModule != nullptr;
}
