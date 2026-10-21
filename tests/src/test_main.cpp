/* @Created On : 2026/10/21
   @Author : 孟源
   @note : 测试总入口 —— 四个套件合成一个 exe, 双击就跑全部。

          每个套件仍然是独立的 .cpp(各自一套 static 用例函数 + 一个 RunXxx 入口),
          这里只负责按顺序调它们、汇总失败数、退出前停一下等人看结果。
*/
#include "../include/test_common.h"

int main() {
    tst::Begin("音频 SDK 测试");

    int nFail = 0;
    nFail += RunPositive();
    nFail += RunNegative();
    nFail += RunBoundary();
    nFail += RunAbnormal();

    return tst::Finalize(nFail);
}
