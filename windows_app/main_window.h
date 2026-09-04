#include <windows.h>


#define BTN_RECORD   1001      // 录制按钮
#define BTN_STOP     1002      // 停止按钮
#define BTN_PLAY     1003      // 播放按钮
#define BTN_PLAY_PAUSE 1004  // 播放/暂停按钮
#define BTN_ENCRYPT 1005  // 加密/取消加密按钮

#define WM_WAVEIN_DONE (WM_USER + 1)

class CMainWindows{
public:
    CMainWindows(HWND hwnd);
    ~CMainWindows();
    void CreateControls(HWND hwnd);
    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    int WINAPI wWinMain(HINSTANCE hInstance,HINSTANCE hPrevInstance,
        LPWSTR lpCmdLine,int nCmdShow);
private:
    HWND m_hBtnRec = NULL;
    HWND m_hBtnPlay = NULL;
    HWND m_hChkEnc  = NULL;
};