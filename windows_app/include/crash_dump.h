#pragma once

/* @Created On : 2026/9/17
   @Author : 孟源
   @note : 崩溃转储 —— 程序崩了自动写出 .dmp, 用 Visual Studio / WinDbg 打开就能看到崩溃现场。

          本头不包含 windows.h, 只有两个函数。
*/

/**
 * @brief 安装崩溃处理器(在程序启动时调一次, 越早越好)
 *
 * @note 能抓到两类崩溃:
 *         ① 空指针 / 除零 / 栈溢出, 以及"throw 出去没人接"的 C++ 异常
 *         ② 显式调用 std::terminate()、noexcept 里抛异常等
 *       转储写到【exe 所在目录】, 文件名 crash_年月日_时分秒.dmp。
 */
void InstallCrashHandler();

/**
 * @brief 摘掉崩溃处理器, 还原回挂之前的样子
 * @note 正常退出时【不需要】调 —— 进程退出系统会统一回收。
 *       它只在"想感知到崩溃后继续跑"或"多套处理器轮流接管"时才有用。
 *       调完之后再崩就不会写 dmp 了。
 */
void UninstallCrashHandler();
