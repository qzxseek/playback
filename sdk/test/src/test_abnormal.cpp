/* @Created On : 2026/9/29
   @Author : 孟源
   @note : 异常测试 —— 调用顺序错、状态不对、对象在使用中被销毁。
          这些不是"坏数据", 而是"坏用法": 每一条都对应一种真实会发生的操作序列
          (连点两次按钮、点停止时其实没在录、播一半关窗口)。

          最后两个用例(播放中/录音中析构)真的会把对象从活着的状态下拆掉。
          它们排在 main 末尾 —— 万一这里把进程带崩, 前面的结果已经打完了。

          需要设备的用例在设备不可用时 [skip]。
*/
#include "../include/test_common.h"

#include "audio_sdk/audio_player.h"
#include "audio_sdk/audio_recorder.h"
#include "audio_sdk/encrypted_format.h"
#include "audio_sdk/wav_validate.h"

#include <windows.h>   // Sleep

// ===========================================================================
// 录音: 调用顺序
// ===========================================================================

/**
 * @brief 重复开始录音: 第二次必须报 DEVICE_BUSY, 且不能把第一次的会话搅乱
 * @note 对应"用户连点两次开始" —— 若不拦, 第一次的缓冲和设备就被顶掉了
 */
static void TestStartTwice() {
    const std::string base = "t_start_twice";

    CAudioRecorder rec;
    rec.SetOutputPath(base.c_str());
    tst::SetEncrypt(rec, true);
    const std::string file = tst::RecFileName(base, rec);

    const AudioSdk::AudioSdkState first = rec.StartRecording();
    if (tst::DeviceUnavailable(first)) { tst::Skip("重复开始录音", "没有可用录音设备"); return; }
    CHECK(first == AudioSdk::AudioSdkState::NONE, "首次 StartRecording 成功");
    if (first != AudioSdk::AudioSdkState::NONE) return;

    ::Sleep(300);

    CHECK(rec.StartRecording() == AudioSdk::AudioSdkState::DEVICE_BUSY,
          "录制中再 StartRecording → DEVICE_BUSY");

    // 计时器按"块"推进(一块 AUDIO_SDK_BLOCK_MS), 不按毫秒实时; 这里只要证明
    // 第一次的会话没被第二次调用清成 0 就算过, 所以下限取一个整块而不是 250ms
    const uint32_t ms = rec.GetRecordedMs();
    std::printf("         被拒后计时器报 %u ms\n", ms);
    CHECK(ms >= AUDIO_SDK_BLOCK_MS, "被拒的第二次调用没有重置第一次的计时");

    CHECK(rec.StopRecording() == AudioSdk::AudioSdkState::NONE, "停止仍正常");

    const std::vector<uint8_t> buf = tst::ReadAll(file);
    CHECK(!buf.empty(), "第一次的录音正常落了盘");
    CHECK(buf.size() > CEncryptedFormat::kAencPrefixSize + sizeof(WavHeader),
          "落盘的不只是空壳");

    std::remove(file.c_str());
}

/**
 * @brief 没开始就停止: 未录制是正常状态而不是错误, 应返回 NONE
 */
static void TestStopWithoutStart() {
    CAudioRecorder rec;
    CHECK(rec.StopRecording() == AudioSdk::AudioSdkState::NONE,
          "未开始就 StopRecording → NONE");
    CHECK(rec.GetRecordedMs() == 0, "未录音时已录时长为 0");
    CHECK(!rec.GetIsPaused(), "未录音时不是暂停态");
    CHECK(rec.GetAencEncrypt(), "新对象的加密开关默认是【开】(与 app 默认落 .aenc 一致)");
}

/**
 * @brief 没开始就暂停/恢复: 不该崩, 也不该把状态搅乱
 */
static void TestPauseWithoutStart() {
    CAudioRecorder rec;
    rec.PauseResumeRecording();
    CHECK(!rec.GetIsPaused(), "未录音时暂停不生效");
    rec.PauseResumeRecording();
    CHECK(!rec.GetIsPaused(), "再来一次也不生效");
    CHECK(rec.StopRecording() == AudioSdk::AudioSdkState::NONE, "之后停止仍返回 NONE");
}

/**
 * @brief 暂停/恢复成对出现: 位置要停住, 恢复后接着走
 * @note 暂停期间的数据不算进已录时长 —— 这是"暂停"和"继续录"的区别所在
 */
static void TestPauseResumeTiming() {
    const std::string base = "t_pause_timing";

    CAudioRecorder rec;
    rec.SetOutputPath(base.c_str());
    tst::SetEncrypt(rec, true);
    const std::string file = tst::RecFileName(base, rec);

    const AudioSdk::AudioSdkState st = rec.StartRecording();
    if (tst::DeviceUnavailable(st)) { tst::Skip("暂停计时", "没有可用录音设备"); return; }
    if (st != AudioSdk::AudioSdkState::NONE) { CHECK(false, "开始录音"); return; }

    ::Sleep(400);
    rec.PauseResumeRecording();
    CHECK(rec.GetIsPaused(), "暂停后 GetIsPaused 为真");

    const uint32_t atPause = rec.GetRecordedMs();
    ::Sleep(600);                                   // 暂停中空转
    const uint32_t afterPause = rec.GetRecordedMs();
    std::printf("         暂停点 %u ms, 空转 600ms 后 %u ms\n", atPause, afterPause);
    CHECK(afterPause <= atPause + 100, "暂停期间计时基本停住(空转 600ms 没被算进去)");

    rec.PauseResumeRecording();
    CHECK(!rec.GetIsPaused(), "恢复后不是暂停态");

    ::Sleep(400);
    const uint32_t afterResume = rec.GetRecordedMs();
    CHECK(afterResume > atPause + 200, "恢复后计时继续往上走");

    CHECK(rec.StopRecording() == AudioSdk::AudioSdkState::NONE, "停止落盘");

    std::remove(file.c_str());
}

/**
 * @brief 停止后再取波形: 最后一块不该因为 Stop 而丢
 * @note 波形环和录音数据是两回事 —— Stop 只影响录音数据, 环里剩的点仍可取
 */
static void TestReadWaveAfterStop() {
    const std::string base = "t_wave_tail";

    CAudioRecorder rec;
    rec.SetOutputPath(base.c_str());
    tst::SetEncrypt(rec, true);
    const std::string file = tst::RecFileName(base, rec);

    const AudioSdk::AudioSdkState st = rec.StartRecording();
    if (tst::DeviceUnavailable(st)) { tst::Skip("停止后取波形", "没有可用录音设备"); return; }
    if (st != AudioSdk::AudioSdkState::NONE) { CHECK(false, "开始录音"); return; }

    ::Sleep(600);
    CHECK(rec.StopRecording() == AudioSdk::AudioSdkState::NONE, "停止落盘");

    float buf[AUDIO_SDK_WAVE_BLOCK_POINTS * 2];
    int pts = 0, batches = 0;
    for (;;) {
        const int n = rec.ReadWave(buf, AUDIO_SDK_WAVE_BLOCK_POINTS);
        if (n <= 0) break;
        pts += n;
        if (++batches > 1000) break;                 // 防死循环
    }
    std::printf("         停止后取到 %d 点 / %d 批\n", pts, batches);
    CHECK(pts > 0, "停止后仍能取到波形点(最后一块没丢)");
    CHECK(batches <= 1000, "取到 0 就停, 不会无尽循环");

    std::remove(file.c_str());
}

// ===========================================================================
// 播放: 调用顺序
// ===========================================================================

/**
 * @brief 先播一个坏文件失败, 再接好文件: 前一次失败不能污染后一次
 * @note 失败路径如果没清干净(比如校验失败却已经把状态标成在播), 这里就会露出来
 */
static void TestPlayInvalidThenValid() {
    const std::string bad  = "t_aiv_bad.bin";
    const std::string good = "t_aiv_good.wav";

    {
        std::vector<uint8_t> b = tst::MakeWav(256);
        std::memcpy(b.data(), "RIFX", 4);
        tst::WriteAll(bad, b);
    }
    tst::WriteAll(good, tst::MakeWav(SAMPLE_RATE / 4));      // 0.25 秒

    CAudioPlayer player;
    CHECK(player.PlayWavFile(bad.c_str()) == AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED,
          "先播坏文件 → FORMAT_NOT_SUPPORTED");
    CHECK(!player.IsPlaying(), "失败后没有残留'正在播放'状态");

    const AudioSdk::AudioSdkState st = player.PlayWavFile(good.c_str());
    if (tst::DeviceUnavailable(st)) {
        tst::Skip("坏文件后接好文件", "没有可用播放设备");
    } else {
        CHECK(st == AudioSdk::AudioSdkState::NONE, "接着播好文件成功");
        CHECK(player.GetTotalPos() > 0, "好文件的数据确实加载了");
        player.StopPlay();
    }

    std::remove(bad.c_str());
    std::remove(good.c_str());
}

/**
 * @brief 没在播就 Stop / Pause / Resume: 都不该崩, 也不该报错
 * @note 对应"用户点停止时其实早播完了"
 */
static void TestStopWithoutPlay() {
    CAudioPlayer player;
    player.StopPlay();
    CHECK(!player.IsPlaying(), "未播放时 StopPlay 不出错, 仍不是播放态");

    player.PausePlay();
    CHECK(!player.IsPaused(), "未播放时 PausePlay 不生效");

    player.ResumePlay();
    CHECK(!player.IsPaused(), "未播放时 ResumePlay 不生效");

    CHECK(player.GetTotalPos() == 0, "未播放时总长为 0");
    CHECK(player.GetPlayPos() == 0, "未播放时位置为 0");
    CHECK(player.GetTotalPosMs() == 0, "未播放时总时长为 0");
    CHECK(player.GetPlayPosMs() == 0, "未播放时已播时长为 0");
}

/**
 * @brief 连续播放两次: 第二次应当干净地替换掉第一次
 * @note 此时第一次还在播 —— 这是"用户不等播完就点了另一个文件"
 */
static void TestPlayTwice() {
    const std::string p1 = "t_twice_1.wav";
    const std::string p2 = "t_twice_2.wav";
    tst::WriteAll(p1, tst::MakeWav(SAMPLE_RATE / 2));     // 0.5 秒
    tst::WriteAll(p2, tst::MakeWav(SAMPLE_RATE / 4));     // 0.25 秒

    CAudioPlayer player;
    const AudioSdk::AudioSdkState a = player.PlayWavFile(p1.c_str());
    if (tst::DeviceUnavailable(a)) {
        tst::Skip("连续播放两次", "没有可用播放设备");
        std::remove(p1.c_str());
        std::remove(p2.c_str());
        return;
    }
    if (a != AudioSdk::AudioSdkState::NONE) {
        CHECK(false, "第一次播放");
        std::remove(p1.c_str());
        std::remove(p2.c_str());
        return;
    }

    ::Sleep(150);                                          // 让它真的播起来
    const AudioSdk::AudioSdkState b = player.PlayWavFile(p2.c_str());
    CHECK(b == AudioSdk::AudioSdkState::NONE, "播放中直接播第二个文件也成功");
    if (b != AudioSdk::AudioSdkState::NONE) {
        CHECK(false, "第二次播放");                        // 不报就成了盲区: 上面那条已经判过失败, 这里再吞掉就没人知道了
        player.StopPlay();
        std::remove(p1.c_str());
        std::remove(p2.c_str());
        return;
    }

    const uint32_t totalMs = player.GetTotalPosMs();
    std::printf("         第二次总时长 %u ms\n", totalMs);
    CHECK(totalMs > 150 && totalMs < 350, "总长已换成第二个文件的(≈250ms)");
    CHECK(player.GetPlayPos() < player.GetTotalPos(), "位置是重新开始的, 不是接着上一个");

    player.StopPlay();
    std::remove(p1.c_str());
    std::remove(p2.c_str());
}

/**
 * @brief 播放中暂停 → 恢复 → 停止: 状态机要走得通
 */
static void TestPauseResumePlay() {
    const std::string path = "t_pause_play.wav";
    tst::WriteAll(path, tst::MakeWav(SAMPLE_RATE * 2));    // 2 秒

    CAudioPlayer player;
    const AudioSdk::AudioSdkState st = player.PlayWavFile(path.c_str());
    if (tst::DeviceUnavailable(st)) {
        tst::Skip("播放暂停恢复", "没有可用播放设备");
        std::remove(path.c_str());
        return;
    }
    if (st != AudioSdk::AudioSdkState::NONE) { CHECK(false, "起播失败"); std::remove(path.c_str()); return; }

    ::Sleep(200);
    player.PausePlay();
    CHECK(player.IsPaused(), "暂停后 IsPaused 为真");
    CHECK(player.IsPlaying(), "暂停不改'正在播放'—— 只是停住不推进");

    ::Sleep(150);                       // 先让已经递进设备的块收尾, 再取暂停点
    const uint32_t atPause = player.GetPlayPos();
    ::Sleep(400);
    const uint32_t afterPause = player.GetPlayPos();
    std::printf("         暂停点 %u, 再等 400ms 后 %u\n", atPause, afterPause);
    CHECK(afterPause == atPause, "暂停期间播放位置不再推进");

    player.ResumePlay();
    CHECK(!player.IsPaused(), "恢复后 IsPaused 转假");
    ::Sleep(300);
    CHECK(player.GetPlayPos() > afterPause, "恢复后位置继续推进");

    player.StopPlay();
    CHECK(!player.IsPlaying(), "停止后不是播放态");
    CHECK(!player.IsPaused(), "停止后暂停标记也一并清掉");

    std::remove(path.c_str());
}

// ===========================================================================
// 资源: 反复建销 / 使用中销毁
// ===========================================================================

/**
 * @brief 反复建销对象: 每次都是全新会话, 不能泄漏设备或线程
 * @note 建销 200 次 —— 泄漏的话句柄数会一路涨, 且析构里若等待线程会明显变慢
 */
static void TestCreateDestroyLoop() {
    const int kTimes = 200;
    const DWORD t0 = ::GetTickCount();
    for (int i = 0; i < kTimes; ++i) {
        CAudioPlayer p;
        CAudioRecorder r;
        (void)p;
        (void)r;
    }
    const DWORD cost = ::GetTickCount() - t0;
    std::printf("         建销 %d 次耗时 %lu ms\n", kTimes, cost);
    CHECK(cost < 5000, "建销 200 次没有明显阻塞(平均每次 < 25ms)");
}

/**
 * @brief 播放之后建销对象: 走过完整播放路径再析构, 不能有残留
 */
static void TestCreateDestroyAfterPlay() {
    const std::string path = "t_cd_after_play.wav";
    tst::WriteAll(path, tst::MakeWav(SAMPLE_RATE / 4));

    for (int i = 0; i < 5; ++i) {
        CAudioPlayer player;
        const AudioSdk::AudioSdkState st = player.PlayWavFile(path.c_str());
        if (tst::DeviceUnavailable(st)) {
            tst::Skip("播放后建销", "没有可用播放设备");
            std::remove(path.c_str());
            return;
        }
        // 起播坏了必须报出来: 否则 5 次空转完, 末尾那句 CHECK(true) 照样是绿的,
        // "走过完整播放路径再析构"这个用例目标根本没被覆盖
        if (st != AudioSdk::AudioSdkState::NONE) {
            CHECK(false, "起播失败");
            std::remove(path.c_str());
            return;
        }
        ::Sleep(50);
    }
    CHECK(true, "连开 5 次播放器再析构, 不崩");

    std::remove(path.c_str());
}

/**
 * @brief 播放中析构对象: 析构要从"正在播"的状态下把设备收干净
 * @note 【这条会真的拆一个活着的播放器】—— 若析构没有先停播放线程,
 *       后面 waveOutClose 就是在关一个还有人用的设备。失败时可能直接把进程带下去。
 *       排在 main 末尾就是为了这个。
 */
static void TestDestroyWhilePlaying() {
    const std::string path = "t_destroy_playing.wav";
    tst::WriteAll(path, tst::MakeWav(SAMPLE_RATE * 5));    // 5 秒, 保证析构时还在播

    {
        CAudioPlayer player;
        const AudioSdk::AudioSdkState st = player.PlayWavFile(path.c_str());
        if (tst::DeviceUnavailable(st)) {
            tst::Skip("播放中析构", "没有可用播放设备");
            std::remove(path.c_str());
            return;
        }
        if (st != AudioSdk::AudioSdkState::NONE) { CHECK(false, "起播失败"); std::remove(path.c_str()); return; }

        CHECK(player.IsPlaying(), "析构前确实还在播");
        ::Sleep(300);                                      // 让播放线程真的跑起来
        // 出作用域 → 析构, 此时还播着
    }
    CHECK(true, "播放中析构对象没崩");

    std::remove(path.c_str());
}

/**
 * @brief 录音中析构对象: 析构要先收尾落盘, 再拆设备和线程
 * @note 【同样会真的拆一个活着的录音器】。~CAudioRecorder 里调了 StopRecording,
 *       所以这次析构之后应当能看到落盘文件。
 */
static void TestDestroyWhileRecording() {
    const std::string base = "t_destroy_rec";
    std::string file;                       // 文件名得在对象销毁前算出来

    {
        CAudioRecorder rec;
        rec.SetOutputPath(base.c_str());
        tst::SetEncrypt(rec, true);
        file = tst::RecFileName(base, rec);
        std::remove(file.c_str());

        const AudioSdk::AudioSdkState st = rec.StartRecording();
        if (tst::DeviceUnavailable(st)) { tst::Skip("录音中析构", "没有可用录音设备"); return; }
        if (st != AudioSdk::AudioSdkState::NONE) { CHECK(false, "开始录音"); return; }

        ::Sleep(500);
        // 出作用域 → 析构, 此时还录着
    }
    CHECK(true, "录音中析构对象没崩");

    const std::vector<uint8_t> buf = tst::ReadAll(file);
    CHECK(!buf.empty(), "析构时把还在录的那一段收了尾并落了盘");
    if (!buf.empty()) {
        CHECK(CEncryptedFormat::IsAencData(buf.data(), buf.size()),
              "落盘的是合法的 .aenc 容器");
        WavValidate v;
        CHECK(v.Validate(buf.data() + CEncryptedFormat::kAencPrefixSize,
                         buf.size() - CEncryptedFormat::kAencPrefixSize),
              "收尾写入的 WAV 头是合法的");
    }

    std::remove(file.c_str());
}

int main() {
    tst::Begin("异常测试");
    RUN(TestStartTwice);
    RUN(TestStopWithoutStart);
    RUN(TestPauseWithoutStart);
    RUN(TestPauseResumeTiming);
    RUN(TestReadWaveAfterStop);
    RUN(TestPlayInvalidThenValid);
    RUN(TestStopWithoutPlay);
    RUN(TestPlayTwice);
    RUN(TestPauseResumePlay);
    RUN(TestCreateDestroyLoop);
    RUN(TestCreateDestroyAfterPlay);

    // ↓ 最后两条会把活着的对象拆掉, 排在末尾: 万一崩了, 上面的结果已经打完
    RUN(TestDestroyWhilePlaying);
    RUN(TestDestroyWhileRecording);
    return tst::Summary("异常");
}
