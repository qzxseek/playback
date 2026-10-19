/* @Created On : 2026/10/16
   @Author : 孟源
   @note : opus 编码接口(压缩格式层)
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "audio_sdk/audio_types.h"
#include "audio_sdk/encrypted_format.h"   // kAopusMagic: 加密魔数跟 kAoggMagic 放一起

#include <opus/opus_types.h>              // 只为回调签名里的 opus_int32, 不拖 opusenc.h 进来

struct OggOpusEnc;

/**
 * @brief Opus 边录边编码落盘器: 开头写好头页 → 边录边编成页追加 → 收尾
 */
class COpusStreamWriter
{
public:
    COpusStreamWriter() = default;
    ~COpusStreamWriter();                // 兜底: 忘了 Finish 也要把尾巴编完

    // 打开文件并写好头页。bEncrypt: true 存 .aopus, false 存明文 .opus
    AudioSdk::AudioSdkState Open(const char* utf8Path, bool bEncrypt);
    // 追加一块 PCM(44100Hz 交错 16bit, 格式见 audio_types.h)。块长随意, 不用自己对齐
    AudioSdk::AudioSdkState Write(void* pcm, size_t n);
    bool Finish();

    uint64_t GetWritten() const { return m_iWritten; }

private:
    // 给 libopusenc 的写回调: 它攒够一页就调一次, 我们在这儿加密落盘。
    // 回调得是普通函数指针, 所以用 user_data 把 this 带进去
    static int OnWrite(void* userData, const unsigned char* p, opus_int32 n);
    static int OnClose(void* userData);

    // 落一段字节(加密在这里做)。返回 false = 写失败
    bool Emit(const void* p, size_t n);
    void CloseState();

    std::ofstream m_file;
    std::vector<uint8_t> m_vecScratch;   // 加密暂存, 见 Emit 的说明
    std::string m_path;                  // 落盘路径
    OggOpusEnc* m_enc = nullptr;         // libopusenc 编码器(内部管着重采样 + 分页)
    bool     m_bOpen = false;
    bool     m_bEncrypt = false;
    uint64_t m_iWritten = 0;             // 已写的载荷字节数, 也是下一段的 XOR 相位
};
