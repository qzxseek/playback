/* @Created On : 2026/9/4
   @Author : 孟源
   @note : Win32 音频 UI 主窗口实现
*/
#include "main_window.h"    // 内含 <windows.h> 与 UNICODE 定义(必须最先)

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
}

/**
 * @brief 创建全部子控件
 * @param hwnd 主窗口句柄
 */
void CMainWindows::CreateControls(HWND hwnd){
    HINSTANCE hInst = GetModuleHandle(NULL);   // 拿程序实例句柄

    m_hBtnRec_Start_Stop = CreateWindowEx(0, L"BUTTON", L"开始录音",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 10, 110, 30,
        hwnd, (HMENU)(INT_PTR)BTN_PLAY_PAUSE, hInst, NULL);
    m_hBtnRecPause = CreateWindowEx(0, L"BUTTON", L"暂停录音",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 126, 10, 110, 30,
        hwnd, (HMENU)(INT_PTR)BTN_RECORD_PAUSE, hInst, NULL);
    m_hChkEnc = CreateWindowEx(0, L"BUTTON", L"加密",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 358, 10, 90, 30,
        hwnd, (HMENU)(INT_PTR)BTN_ENCRYPT, hInst, NULL);

    m_hBtnOpen = CreateWindowEx(0, L"BUTTON", L"打开文件...",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 70, 110, 30,
        hwnd, (HMENU)(INT_PTR)BTN_OPEN_FILE, hInst, NULL);
    m_hBtnPlay_Start_Stop = CreateWindowEx(0, L"BUTTON", L"播放",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 126, 70, 80, 30,
        hwnd, (HMENU)(INT_PTR)BTN_PLAY, hInst, NULL);
    m_hBtnPlayPause = CreateWindowEx(0, L"BUTTON", L"暂停播放",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 212, 70, 110, 30,
        hwnd, (HMENU)(INT_PTR)BTN_PLAY_PAUSE, hInst, NULL);
    
    EnableWindow(m_hBtnRecPause, FALSE);                    // 禁用暂停录音按钮
    EnableWindow(m_hBtnPlay_Start_Stop, FALSE);             // 禁用播放按钮
    EnableWindow(m_hBtnPlay_Start_Stop, FALSE);             // 禁用播放/停止按钮
    EnableWindow(m_hBtnPlayPause, FALSE);                   // 禁用继续播放/暂停按钮
}

/**
 * @brief 窗口过程(静态成员, 注册给系统的回调)
 *   转发到 g_pMain 实例的 HandleMessage; g_pMain 为空时走默认处理。
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
    case WM_CREATE:                 // 窗口创建成功 → 建控件
        OnCreate(hwnd);
        return 0;

    case WM_COMMAND: {              // 控件被点击
        const int iId = LOWORD(wParam);       // 控件 ID
        const int iNotify = HIWORD(wParam);   // 通知码
        if (iNotify == BN_CLICKED)
            OnCommand(iId);
        return 0;
    }

    case WM_DESTROY:                // 窗口销毁 → 结束消息循环
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
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

    // 有文件了, 点亮播放按钮(若当前没在播)
    if (m_hBtnPlay_Start_Stop)
        EnableWindow(m_hBtnPlay_Start_Stop, TRUE);

    return true;
}

/**
 * @brief 开始/继续录音
 * @param cRecorder 录制器实例
 */
void CMainWindows::AudioStartRec(CAudioRecorder cRecorder){
    auto res = cRecorder.StartRecording();
        if (res != AudioSdk::AudioSdkState::NONE)
        {
            m_isRecording = true;
            SetWindowTextW(m_hBtnRec_Start_Stop, L"停止录音");
            SetWindowTextW(m_hBtnRecPause, L"暂停录音");
            EnableWindow(m_hChkEnc, !m_isRecording);                // 禁用加密复选框
            EnableWindow(m_hBtnOpen, !m_isRecording);               // 禁用打开文件按钮
            EnableWindow(m_hBtnPlay_Start_Stop, !m_isRecording);    // 禁用播放/停止按钮
            EnableWindow(m_hBtnPlayPause, !m_isRecording);          // 禁用继续播放/暂停按钮
        }
}

/**
 * @brief 暂停/继续录音
 * @param cRecorder 录制器实例
 */
void CMainWindows::AudioPauseResumeRec(CAudioRecorder cRecorder){
    cRecorder.PauseResumeRecording();
    auto isPaused = cRecorder.GetIsPaused();
    if (isPaused){
        m_isRecording = false;
        SetWindowTextW(m_hBtnRecPause, L"继续录音");
        EnableWindow(m_hChkEnc, !m_isRecording);                // 启用加密复选框
    }
    else{
        m_isRecording = true;
        SetWindowTextW(m_hBtnRecPause, L"暂停录音");
        EnableWindow(m_hChkEnc, !m_isRecording);                // 禁用加密复选框
    }
}

/**
 * @brief 停止录音
 * @param cRecorder 录制器实例
 */
void CMainWindows::AudioStopRec(CAudioRecorder cRecorder){
    auto res = cRecorder.StopRecording();
    if (res != AudioSdk::AudioSdkState::NONE)
    {
        m_isRecording = false;
        SetWindowTextW(m_hBtnRec_Start_Stop, L"开始录音");
        SetWindowTextW(m_hBtnRecPause, L"暂停录音");
        EnableWindow(m_hChkEnc, !m_isRecording);                // 启用加密复选框
        EnableWindow(m_hBtnOpen, !m_isRecording);               // 启用打开文件按钮        
    }
}

/**
 * @brief 暂停/继续播放
 * @param cPlayer 播放器实例
 */
void CMainWindows::AudioStartPlay(CAudioPlayer cPlayer){
    if (!m_curFile.empty()){
            const AudioSdk::AudioSdkState res = cPlayer.PlayWavFile(m_curFile.c_str());
            switch (res){
            case AudioSdk::AudioSdkState::NONE:{
                SetWindowTextW(m_hBtnPlay_Start_Stop, L"播放中");
                EnableWindow(m_hBtnPlayPause, TRUE);
                EnableWindow(m_hBtnRec_Start_Stop, FALSE);
                EnableWindow(m_hBtnRecPause, FALSE);
                EnableWindow(m_hChkEnc, FALSE);
                EnableWindow(m_hBtnOpen, FALSE);
            }
            case AudioSdk::AudioSdkState::INVALID_PARAMETER:{
                MessageBoxW(m_hwnd, L"文件路径错误", L"播放",
                            MB_OK | MB_ICONERROR);
                break;
            }
            case AudioSdk::AudioSdkState::FILE_OPEN_FAILED:{
                MessageBoxW(m_hwnd, L"文件打开失败", L"播放",
                            MB_OK | MB_ICONERROR);
                break;
            }
            case AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED:{
                MessageBoxW(m_hwnd, L"文件格式不支持", L"播放",
                            MB_OK | MB_ICONERROR);
            }
            case AudioSdk::AudioSdkState::FILE_READ_FAILED:{
                MessageBoxW(m_hwnd, L"文件读取失败", L"播放",
                            MB_OK | MB_ICONERROR);
                break;
            }
            case AudioSdk::AudioSdkState::DEVICE_BUSY:{
                MessageBoxW(m_hwnd, L"设备忙", L"播放",
                            MB_OK | MB_ICONERROR);
                break;
            }
            case AudioSdk::AudioSdkState::DEVICE_NOT_FOUND:{
                MessageBoxW(m_hwnd, L"设备无法打开", L"播放",
                            MB_OK | MB_ICONERROR);
                break;
            }
            default:{
                MessageBoxW(m_hwnd, L"请先点【打开文件】选择音频", L"播放", 
                    MB_OK | MB_ICONINFORMATION);
            }
        }
    }
}

/**
 * @brief 暂停/继续播放
 * @param cPlayer 播放器实例
 */
void CMainWindows::AudioPauseResumePlay(CAudioPlayer cPlayer){
    // 暂停
    if (cPlayer.GetIsPaused()){
        cPlayer.Pause();
    }
    // 继续播放
    else{
        cPlayer.Resume();
    }
}

/**
 * @brief 停止播放
 * @param cPlayer 播放器实例
 */
void CMainWindows::AudioStopPlay(CAudioPlayer cPlayer){
    cPlayer.Stop();
    EnableWindow(m_hBtnPlayPause, FALSE);
    EnableWindow(m_hBtnRec_Start_Stop, TRUE);
    EnableWindow(m_hChkEnc, TRUE);
    EnableWindow(m_hBtnOpen, TRUE);
}

/**
 * @brief WM_COMMAND: 根据控件 ID 分发按钮点击
 * @param iId 控件 ID
 */
void CMainWindows::OnCommand(int iId){
    CAudioRecorder cRecorder;          // 录制器实例
    CAudioPlayer   cPlayer;            // 播放器实例
    switch (iId){
    case BTN_RECORD:                  // 开始录制
        AudioStartRec(cRecorder);
        break;
    case BTN_RECORD_PAUSE:            // 暂停/继续录音
        AudioPauseResumeRec(cRecorder);
        break;
    case BTN_STOP:        // 停止录音
        AudioStopRec(cRecorder);
        break;
    case BTN_OPEN_FILE:{        // 打开文件
        bool res = OpenFileDialog(m_hwnd);
        if (!res) MessageBoxW(m_hwnd, L"打开文件失败",
             L"打开文件", MB_OK | MB_ICONERROR);
        break;
    }
    case BTN_PLAY:        // 播放当前打开的文件
        AudioStartPlay(cPlayer);          // 开始播放
        break;
    case BTN_PLAY_PAUSE:  // 播放/暂停
        AudioPauseResumePlay(cPlayer);
        break;
    case BTN_STOP_PLAY:   // 停止播放
        AudioStopPlay(cPlayer);
        break;
    case BTN_ENCRYPT:     // 加密/取消加密
        break;
    }
}
