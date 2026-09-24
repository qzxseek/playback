#pragma once

/* @Created On : 2026/9/17
   @Author : 孟源
   @note : 崩溃转储 —— 程序崩了自动写出 .dmp, 用 Visual Studio / WinDbg 打开就能看到崩溃现场
*/


void InstallCrashHandler();

void UninstallCrashHandler();
