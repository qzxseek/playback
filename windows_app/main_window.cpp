/* @Created On : 2026/9/4
   @Author : 孟源
   @note : Win32 音频 UI 主窗口实现
*/
#include "main_window.h"    

#include <cstring>          
#include <fstream>          

// g_pMain 声明在 main.cpp(WinMain 里 new 出来并赋值)。
// 静态 WndProc 需要它把消息转发回实例。
extern CMainWindows* g_pMain;

/**
 * @brief 判断某文件前 4 字节是否 "AENC"(加密容器), 用于提示
 * @param path 文件路径
 * @return true 是加密文件, false 否是
 */
static bool IsAencFileByPath(const wchar_t* path){
    std::ifstream f(path, std::ios::binary);
    char head[4] = {};
    f.read(head, 4);
    return f.gcount() == 4 && std::memcmp(head, "AENC", 4) == 0;
}

/**
 * @brief 读取音频文件的"每秒字节数"(byteRate), 用于把 字节→时间 换算。
 * @param path 文件路径
 * @return 每秒字节数
 */
static DWORD ReadByteRate(const wchar_t* path){
    std::ifstream f(path, std::ios::binary);
    char raw[64] = {};
    f.read(raw, sizeof(raw));
    if (f.gcount() < 50)
        return 0;

    size_t off = 0;
    if (std::memcmp(raw, "AENC", 4) == 0)
        off = 6;                       // 剥掉 AENC + version

    // WavHeader 内 byteRate 偏移: RIFF(4)+size(4)+WAVE(4)+fmt(4)+size(4)+
    // fmtTag(2)+channels(2)+sampleRate(4) = 28
    DWORD byteRate = 0;
    std::memcpy(&byteRate, raw + off + 28, sizeof(byteRate));
    return byteRate;
}


CMainWindows::CMainWindows(){
}

CMainWindows::~CMainWindows(){
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
                    m_player.Seek(m_playPosBytes);        // 点哪跳哪 / 拖到哪跳到哪
                    m_playPosBytes = m_player.GetPlayPos();   // Seek 内部做了帧对齐
                    UpdateProgressUI();
                }
            }
            return 0;

        case WM_PAINT: {                     // 绘制进度条
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            DrawProgress(hdc);
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
    switch (iId){
    case BTN_RECORD_START_STOP:            // 开始 / 停止录音(同一按钮切换文字)
        AudioStartRec();
        break;
    case BTN_RECORD_PAUSE:                 // 暂停/继续录音
        AudioPauseResumeRec();
        break;
    case BTN_ENCRYPT:                      // 加密复选框(点击后勾选已自动翻转)
        if (!m_isRecording)
            m_recorder.SetAencEncrypt();   // 翻转 recorder 内部加密状态
        // 让勾选显示与 recorder 真实状态保持一致
        SendMessageW(m_hChkEnc, BM_SETCHECK,
                     m_recorder.GetAencEncrypt() ? BST_CHECKED : BST_UNCHECKED, 0);
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
void CMainWindows::AudioStartRec(){
    if (!m_isRecording){
        // ---- 开始录音 ----
        if (m_recorder.StartRecording() != AudioSdk::AudioSdkState::NONE){
            MessageBoxW(m_hwnd, L"打开录音设备失败", L"录音", MB_OK | MB_ICONERROR);
            return;
        }
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
        m_recorder.StopRecording();
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
        MessageBoxW(m_hwnd, m_recorder.GetAencEncrypt()
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
    m_recorder.PauseResumeRecording();
    m_recPaused = m_recorder.GetIsPaused();
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

    // 可选: 在窗口标题上显示当前文件, 直观反馈选到了什么
    std::wstring title = L"Win32 音频播放器 - ";
    title += file;
    if (IsAencFileByPath(file)) title += L"  (加密 .aenc)";
    else title += L"  (明文)";
    SetWindowTextW(m_hwnd, title.c_str());

    if (!m_isRecording && !m_isPlaying)
        EnableWindow(m_hBtnPlay_Start_Stop, TRUE);
    return true;
}

/**
 * @brief 更新录音时间文字
 * @param tenths 录音时长, 单位 0.1 秒
 */
void CMainWindows::UpdateRecTimeUI(DWORD tenths){
    wchar_t text[32];
    wsprintfW(text, L"录音时长: %02u:%02u.%u",
              tenths / 600, (tenths / 10) % 60, tenths % 10);   // mm:ss.d
    SetWindowTextW(m_hLblRecTime, text);
}

// --------------播放相关------------------

/**
 * @brief 开始 / 停止播放(按当前状态)
 */
void CMainWindows::AudioStartStopPlay(){
    // ---- 停止播放 ----
    if (m_isPlaying) {
        m_player.Stop();
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

    const AudioSdk::AudioSdkState res = m_player.PlayWavFile(m_curFile.c_str());
    if (res != AudioSdk::AudioSdkState::NONE){
        const wchar_t* msg = L"播放失败";
        switch (res){
            case AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED:msg = L"格式不支持"; break;
            case AudioSdk::AudioSdkState::FILE_OPEN_FAILED:msg = L"文件打开失败"; break;
            case AudioSdk::AudioSdkState::DEVICE_BUSY:msg = L"设备被占用"; break;
            case AudioSdk::AudioSdkState::DEVICE_NOT_FOUND:msg = L"找不到播放设备"; break;
            default: break;
        }
        MessageBoxW(m_hwnd, msg, L"播放", MB_OK | MB_ICONERROR);
        return;
    }

    m_isPlaying  = true;
    m_playPaused = false;
    m_dragging   = false;
    m_playTotalBytes = m_player.GetTotalPos();
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
        m_player.Resume();
        m_playPaused = false;
        SetWindowTextW(m_hBtnPlayPause, L"暂停");
    }
    else{
        m_player.Pause();
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
}

/**
 * @brief 定时器: 问播放器播到哪 → 更新显示; 并检测"自然播完"复位
 */
void CMainWindows::OnTimerTick(){
    // 录音中: 已录字节 ×10 ÷ 每秒字节数 = 十分之一秒数
    if (m_isRecording){
        const size_t bytes = m_recorder.GetRecordedBytes();
        const size_t byteRate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
        UpdateRecTimeUI(static_cast<DWORD>(bytes * 10 / byteRate));
    }

    if (!m_isPlaying) return;                     // 没在播就不刷
    if (m_dragging) return;                        // 拖动预览中, 不抢位置

    m_playPosBytes   = m_player.GetPlayPos();
    m_playTotalBytes = m_player.GetTotalPos();

    UpdateProgressUI();

    // 自然播完: 之前还在播, 现在播放器自己停了(不是暂停)
    if (!m_playPaused && !m_player.IsPlaying()){
        m_playPosBytes = m_playTotalBytes;
        UpdateProgressUI();
        AudioStartStopPlay();
    }
}

/**
 * @brief 更新进度条显示
 */
void CMainWindows::UpdateProgressUI(){
    // 时间文字: 字节 → mm:ss.d(每秒字节数从文件头读, 读不到用工程默认 88200)
    DWORD byteRate = ReadByteRate(m_curFile.c_str());
    if (byteRate == 0) byteRate = 88200;

    const unsigned __int64 tPosTenth = (unsigned __int64)m_playPosBytes * 10 / byteRate;
    const unsigned __int64 tTotTenth = (unsigned __int64)m_playTotalBytes * 10 / byteRate;

    wchar_t now[16], tot[16], text[48];
    wsprintfW(now, L"%02u:%02u.%u",
              (UINT)(tPosTenth / 600), (UINT)((tPosTenth / 10) % 60), (UINT)(tPosTenth % 10));
    wsprintfW(tot, L"%02u:%02u.%u",
              (UINT)(tTotTenth / 600), (UINT)((tTotTenth / 10) % 60), (UINT)(tTotTenth % 10));
    wsprintfW(text, L"%s / %s", now, tot);
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
