/* @Created On : 2026/10/12
   @Author : 孟源
   @note : ogg/vorbis 编解码实现(压缩格式层)
*/
#include "audio_sdk/audio_types.h"
#include "audio_sdk/ogg_format.h"
#include "audio_sdk/encrypted_format.h"
#include "audio_sdk/logging.h"
#include "audio_sdk/waveform.h"     // CWaveform::ComputePeaks(BuildPeaks 用)

#include <algorithm>        // std::min
#include <cerrno>           // errno / ENOSPC: 写失败时判是不是盘满
#include <cstdio>           // SEEK_SET / SEEK_CUR / SEEK_END(定位回调要用)
#include <cstring>
#include <exception>
#include <filesystem>
#include <new>
#include <random>           // 生成随机数

static constexpr const char* kTag = "ogg";     // 日志来源标记

// 每帧字节数(单声道 16bit = 2)。录音器给的块一定是整帧的, 这里只做一次兜底
static constexpr size_t kFrameBytes = CHANNELS * (BITS_PER_SAMPLE / 8);

static constexpr float kQuality = 0.4f;

/**
 * @brief 生成随机流的身份证号
 * @return 随机身份证号
 */
inline int generateOggSerial() {
    static const int serial = [] {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(1, 0x7FFFFFFF);
        return dist(gen);
    }();
    return serial;
}

static const int kOggSerial = generateOggSerial();


// -----------编码器-----------

/**
 * @brief 判一段内存开头是不是明文 Ogg 容器
 * @param data 内存起始地址(通常是文件开头)
 * @param size 数据长度
 */
bool COggFormat::IsOggData(const uint8_t* data, size_t size){
    return data && size >= kMagicSize && std::memcmp(data, kOggMagic, kMagicSize) == 0;
}

/**
 * @brief 判一段内存开头是不是加密 Ogg 容器
 */
bool COggFormat::IsAoggData(const uint8_t* data, size_t size){
    return data && size >= CEncryptedFormat::kAoggMagicSize &&
           std::memcmp(data, CEncryptedFormat::kAoggMagic,
                       CEncryptedFormat::kAoggMagicSize) == 0;
}


COggStreamWriter::~COggStreamWriter(){
    Finish();                       
}

/**
 * @brief 关文件 + 释放五个编码器结构体
 */
void COggStreamWriter::CloseState(){
    ogg_stream_clear(&m_os);        // 顺序: 与 Open 里的 _init 逆着来
    vorbis_block_clear(&m_vb);
    vorbis_dsp_clear(&m_vd);
    vorbis_comment_clear(&m_vc);
    vorbis_info_clear(&m_vi);

    if (m_file.is_open())
        m_file.close();
    m_bOpen = false;
}

/**
 * @brief 打开文件并写好头三个页(标识头 / 注释 / 码本)
 * @param utf8Path 输出文件路径(UTF-8)
 * @param bEncrypt true 存成 .aogg 加密容器, false 存成明文 .ogg
 * @return 操作状态
 */
AudioSdk::AudioSdkState COggStreamWriter::Open(const char* utf8Path, bool bEncrypt){
    Finish();                       // 上一轮没关干净先收尾

    if (!utf8Path)
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;

    try {
        m_file.clear();
        m_file.open(std::filesystem::u8path(utf8Path),
                    std::ios::out | std::ios::binary | std::ios::trunc);
        if (!m_file.is_open())
            return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;   // 还没开成, 没什么可清的

        m_bEncrypt = bEncrypt;
        m_bBroken  = false;             // 新文件, 上一轮的写坏状态不带过来
        m_iWritten = 0;
        m_path = utf8Path;      

        if (bEncrypt) {             // .aogg 壳: 魔数 + 版本排在压缩流前面, 明文
            m_file.write(CEncryptedFormat::kAoggMagic, CEncryptedFormat::kAoggMagicSize);
            const uint16_t version = CEncryptedFormat::kAencVersion;
            m_file.write(reinterpret_cast<const char*>(&version), sizeof(version));
            if (!m_file.good()){
                CloseState();
                return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
            }
        }

        // 编码器初始化
        vorbis_info_init(&m_vi);
        if (vorbis_encode_init_vbr(&m_vi, CHANNELS, SAMPLE_RATE, kQuality) != 0) {
            AUDIO_LOG_ERROR(kTag, "编码器初始化失败: %d 声道 %d Hz 质量 %.2f",
                            CHANNELS, SAMPLE_RATE, (double)kQuality);
            CloseState();
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        }
        std::string sTag = "ENCODER";
        std::string sComment = "audio_sdk";
        vorbis_comment_init(&m_vc);
        vorbis_comment_add_tag(&m_vc, sTag.c_str(), sComment.c_str());
        vorbis_analysis_init(&m_vd, &m_vi);
        vorbis_block_init(&m_vd, &m_vb);
        if (ogg_stream_init(&m_os, kOggSerial) != 0){
            CloseState();
            return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
        }      // 只在分配失败时非 0
            
        // 头两页
        ogg_packet hId{}, hComm{}, hCode{};
        if (vorbis_analysis_headerout(&m_vd, &m_vc, &hId, &hComm, &hCode) != 0){
            CloseState();
            return AudioSdk::AudioSdkState::UNKNOWN_ERROR;
        }
        ogg_stream_packetin(&m_os, &hId);
        ogg_stream_packetin(&m_os, &hComm);
        ogg_stream_packetin(&m_os, &hCode);

        ogg_page og;
        while (ogg_stream_flush(&m_os, &og)) {
            if (!Emit(og.header, static_cast<size_t>(og.header_len)) ||
                !Emit(og.body, static_cast<size_t>(og.body_len))){
                CloseState();
                return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
            }
        }

        m_file.flush();             // 头两页立刻落盘: 中途断电也留个认得出的文件
        if (!m_file.good()){
            CloseState();
            return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
        }
    }
    catch (const std::bad_alloc&) {
        CloseState();
        return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
    }
    catch (const std::exception&) {
        CloseState();
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
    }

    m_bOpen = true;                 // 到这里才真的可用
    return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 落一段 Ogg 字节(加密在这里做)
 * @param p 要写的字节指针
 * @param n 要写字节数
 * @return 是否写成功
 */
bool COggStreamWriter::Emit(const void* p, size_t n){
    if (m_bBroken)
        return false;               // 相位已经不可信, 收尾时 flush 也别再往盘上写
    if (!m_file.is_open())
        return false;
    if (n == 0)
        return true;                // 空 body 的页是合法的, 不是写失败

    const char* src = static_cast<const char*>(p);

    if (m_bEncrypt) {
        try {
            m_vecScratch.resize(n);
        } catch (const std::bad_alloc&) {
            return false;           // 音频线程上不能让异常穿出去
        }
        std::memcpy(m_vecScratch.data(), p, n);
        CEncryptedFormat::XorCrypt(m_vecScratch.data(), n, static_cast<size_t>(m_iWritten));
        src = reinterpret_cast<const char*>(m_vecScratch.data());
    }

    m_file.write(src, static_cast<std::streamsize>(n));
    if (!m_file.good()) {
        m_bBroken = true;
        const int err = errno;
        std::error_code ec;
        const std::filesystem::space_info sp = std::filesystem::space(m_path, ec);
        const unsigned long long availMB =
            ec ? 0ULL : static_cast<unsigned long long>(sp.available / 1048576ULL);

        AUDIO_LOG_ERROR(kTag, "写失败 \"%s\": errno=%d%s%s, 已写 %llu 字节, 本次 %zu 字节, 剩余 %lluMB",
                        m_path.c_str(), err,
                        err == ENOSPC ? " [ENOSPC 磁盘空间不足]" : "",
                        ec ? " [拿不到剩余空间]" : "",
                        static_cast<unsigned long long>(m_iWritten), n, availMB);
        return false;
    }

    m_iWritten += n;
    return true;
}

/**
 * @brief 把编码管道里的东西全推出去: 分析 → 分块 → 定型 → 打包 → 分页 → 落盘
 * @return false = 有字节没写进去(文件已不可信, 调用方应当停止录音)
 */
bool COggStreamWriter::Drain(){
    ogg_packet op{};
    ogg_page   og{};

    while (vorbis_analysis_blockout(&m_vd, &m_vb) == 1) {
        vorbis_analysis(&m_vb, nullptr);        // 定型: 这块 PCM → 频域 → 量化
        vorbis_bitrate_addblock(&m_vb);         // 交给码率控制, 它决定这个包占多少比特

        while (vorbis_bitrate_flushpacket(&m_vd, &op)) {
            ogg_stream_packetin(&m_os, &op);
            while (ogg_stream_pageout(&m_os, &og)) {
                if (!Emit(og.header, static_cast<size_t>(og.header_len)) ||
                    !Emit(og.body,   static_cast<size_t>(og.body_len)))
                    return false;
            }
        }
    }
    return true;
}

/**
 * @brief 追加一块 PCM: 转成浮点喂给编码器, 再把编码结果推到文件
 * @param pcm 交错排列的 16bit PCM(格式见 audio_types.h)
 * @param n 字节数
 * @return NONE = 已写进文件; FILE_OPEN_FAILED = 没打开; FILE_WRITE_FAILED = 写不进去;
 *         INVALID_PARAMETER = 不是整帧(会与写入器对不齐, 只能拒)
 */
AudioSdk::AudioSdkState COggStreamWriter::Write(void* pcm, size_t n){
    if (!m_bOpen)
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;
    if (m_bBroken)
        return AudioSdk::AudioSdkState::FILE_WRITE_FAILED;   // 已经写坏过, 别再用错相位往下写
    if (n == 0)
        return AudioSdk::AudioSdkState::NONE;      // 空块不算失败(录音刚起可能给出 0 字节的块)
    if (!pcm || n % kFrameBytes != 0)
        return AudioSdk::AudioSdkState::INVALID_PARAMETER;

    const int iSamples = static_cast<int>(n / kFrameBytes);

    // 转成 [-1, 1] 的浮点, 顺便去交错: vorbis 只吃 float, 而且要的是"每个声道一段连续内存", 不能是交错排布
    const int16_t* const src = static_cast<const int16_t*>(pcm);
    float** const buf = vorbis_analysis_buffer(&m_vd, iSamples);
    if (!buf){
        // 内部缓冲重分配失败时返回 NULL。不判空就直接 buf[ch] 是空指针写
        AUDIO_LOG_ERROR(kTag, "vorbis_analysis_buffer 返回 NULL(%d 采样) -> OUT_OF_MEMORY", iSamples);
        m_bBroken = true;           // 管道状态已经不可信, 别再往下喂
        return AudioSdk::AudioSdkState::OUT_OF_MEMORY;
    }
    for (int ch = 0; ch < CHANNELS; ++ch) {
        float* const dst = buf[ch];
        for (int i = 0; i < iSamples; ++i)
            dst[i] = src[i * CHANNELS + ch] / 32768.0f;
    }
    vorbis_analysis_wrote(&m_vd, iSamples);        

    return Drain() ? AudioSdk::AudioSdkState::NONE
                   : AudioSdk::AudioSdkState::FILE_WRITE_FAILED;
}

/**
 * @brief 收尾: 逼出管道残渣 + 写最后一个页(EOS) + 关文件
 * @return 落盘是否成功
 */
bool COggStreamWriter::Finish(){
    if (!m_bOpen)
        return true;                // 没打开过、或已经收过尾 —— 没有残文件
    // 中途写坏过: 后面的页要么没落盘、要么相位错位, 这个文件已经不是合法容器了
    bool bSuccess = !m_bBroken;

    // 告诉编码器"没数据了",它会拿这个把 MDCT 窗口里剩的样本补完再吐出来
    vorbis_analysis_wrote(&m_vd, 0);
    bSuccess = Drain() && bSuccess;

    // Drain 里的 pageout 只吐"攒够一页"的, 尾巴上不满一页的部分要靠 flush 强制推出
    ogg_page og;
    while (ogg_stream_flush(&m_os, &og)) {
        if (!Emit(og.header, static_cast<size_t>(og.header_len)) ||
            !Emit(og.body,   static_cast<size_t>(og.body_len))) {
            bSuccess = false;
            break;
        }
    }

    m_file.flush();
    bSuccess = m_file.good() && bSuccess;
    if (!bSuccess){
        CloseState();
        AUDIO_LOG_ERROR(kTag, "编码器落盘失败，路径: %s", m_path.c_str());
        return bSuccess;
    }
    AUDIO_LOG_ERROR(kTag, "编码器落盘成功，路径: %s", m_path.c_str());
    CloseState();
    return bSuccess;
}

// -----------解码器-----------

COggStreamReader::~COggStreamReader(){
    Close();                            // 兜底: 忘了 Close 也要把 ov_clear 补上
}

/**
 * @brief 关闭解码器: 清空状态 + 关文件 + 清 ov_clear 结构体
 */
void COggStreamReader::Close(){
    if (m_open) {
        ov_clear(&m_vf);                // 内部会调 CloseCb(空实现), 不碰我们的 m_file
        m_open = false;
    }
    if (m_file.is_open())
        m_file.close();

    m_channels = m_rate = 0;
    m_blockAlign = 0;
    m_totalBytes = 0;
    m_prefix = m_payload = 0;
    m_encrypt = false;
}

/**
 * @brief 读入口(照 fread 的语义: 返回读到了几个完整单元)
 * @param ptr 读到的内存指针
 * @param size 每个单元的字节数
 * @param nmemb 要读的单元数
 * @param ds 指向 COggStreamReader 的指针
 * @return 读到了几个完整单元
 */
size_t COggStreamReader::ReadCb(void* ptr, size_t size, size_t nmemb, void* ds){
    COggStreamReader* const self = static_cast<COggStreamReader*>(ds);
    if (!self || size == 0 || nmemb == 0 || !self->m_file.is_open())
        return 0;

    std::ifstream& f = self->m_file;
    const size_t want = size * nmemb;

    const std::streamoff before = f.tellg();

    f.read(reinterpret_cast<char*>(ptr), static_cast<std::streamsize>(want));
    const size_t got = static_cast<size_t>(f.gcount());
    if (got == 0)
        return 0;

    // 解密
    if (self->m_encrypt && before >= 0)
        CEncryptedFormat::XorCrypt(static_cast<uint8_t*>(ptr), got,
                                   static_cast<size_t>(before) - self->m_prefix);

    return got / size;
}

/**
 * @brief 定位入口。offset 是载荷内偏移, 要自己加上前缀才是文件里的真实位置
 * @param ds 指向 COggStreamReader 的指针
 * @param offset 要跳的偏移量
 * @param whence 定位方式
 * @return 0 = 成功(跟 fseek 一致)
 */
int COggStreamReader::SeekCb(void* ds, ogg_int64_t offset, int whence){
    COggStreamReader* const self = static_cast<COggStreamReader*>(ds);
    if (!self || !self->m_file.is_open())
        return -1;

    self->m_file.clear();

    std::streamoff target = 0;
    switch (whence) {
    // 定位到载荷内偏移 offset
    case SEEK_SET:
        target = static_cast<std::streamoff>(self->m_prefix) + offset;
        break;
    // 定位到当前位置 + offset
    case SEEK_CUR:
        target = self->m_file.tellg() + static_cast<std::streamoff>(offset);
        break;
    // 定位到文件结束位置 + offset
    case SEEK_END:
        // 载荷在文件里的结束位置 = 前缀 + 载荷长 = 文件总长
        target = static_cast<std::streamoff>(self->m_prefix + self->m_payload) + offset;
        break;
    default:
        return -1;
    }
    if (target < 0)
        return -1;

    self->m_file.seekg(target, std::ios::beg);
    return self->m_file.good() ? 0 : -1;
}

/**
 * @brief 关闭入口 —— 故意留空: 文件归 COggStreamReader 自己管(析构里关)
 */
int COggStreamReader::CloseCb(void* /*ds*/){
    return 0;
}

/**
 * @brief 报位置入口。同样报的是载荷内偏移, 不能把前缀算进去
 */
long COggStreamReader::TellCb(void* ds){
    COggStreamReader* const self = static_cast<COggStreamReader*>(ds);
    if (!self)
        return -1;
    const std::streamoff pos = self->m_file.tellg();
    if (pos < 0)
        return -1;
    return static_cast<long>(pos - static_cast<std::streamoff>(self->m_prefix));
}

/**
 * @brief 打开 Ogg 文件并读出头信息(.ogg / .aogg 通吃, 看前缀自动决定要不要解密)
 * @param utf8Path 已落盘的文件路径(UTF-8)
 * @return 操作状态
 */
AudioSdk::AudioSdkState COggStreamReader::Open(const char* utf8Path){
    Close();                            // 上一轮没关干净先收尾(与写入器的 Open 一致)

    if (!utf8Path)
        return AudioSdk::AudioSdkState::INVALID_PARAMETER;

    try {
        m_file.clear();
        // 与写入侧同一个理由: 中文路径必须过 u8path, 否则按代码页 936 解出一堆乱码
        m_file.open(std::filesystem::u8path(utf8Path), std::ios::in | std::ios::binary);
        if (!m_file.is_open())
            return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;

        // 总长先量出来, 后面算 m_payload 要用
        m_file.seekg(0, std::ios::end);
        const std::streamoff total = m_file.tellg();
        if (total < static_cast<std::streamoff>(CEncryptedFormat::kAencPrefixSize)) {
            m_file.close();
            return AudioSdk::AudioSdkState::FILE_READ_FAILED;   // 太短, 连魔数都放不下
        }

        // ---- 认前缀: 是加密的还是明文的 ----
        uint8_t head[CEncryptedFormat::kAencPrefixSize] = {};
        m_file.seekg(0, std::ios::beg);
        m_file.read(reinterpret_cast<char*>(head), sizeof(head));
        if (m_file.gcount() != static_cast<std::streamsize>(sizeof(head))) {
            m_file.close();
            return AudioSdk::AudioSdkState::FILE_READ_FAILED;        // 读取失败, 文件包过小，损坏
        }

        if (COggFormat::IsAoggData(head, sizeof(head))) {
            m_encrypt = true;
            m_prefix  = CEncryptedFormat::kAencPrefixSize;      // .aogg = 魔数4 + 版本2
        }
        else if (COggFormat::IsOggData(head, sizeof(head))) {
            m_encrypt = false;
            m_prefix  = 0;
        }
        else {
            // 魔数对不上
            m_file.close();
            AUDIO_LOG_ERROR(kTag, "不是 Ogg 容器: %s", utf8Path);
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        }

        m_payload = static_cast<size_t>(total) - m_prefix;

        // 把读指针摆到载荷起点, 再交给 libvorbisfile
        m_file.seekg(static_cast<std::streamoff>(m_prefix), std::ios::beg);

        ov_callbacks cbs{};
        cbs.read_func  = &COggStreamReader::ReadCb;
        cbs.seek_func  = &COggStreamReader::SeekCb;
        cbs.close_func = &COggStreamReader::CloseCb;
        cbs.tell_func  = &COggStreamReader::TellCb;

        // datasource 就是 this —— 四个回调靠它转回来拿到 m_file / m_encrypt / m_prefix
        if (ov_open_callbacks(this, &m_vf, nullptr, 0, cbs) != 0) {
            // 走到这儿说明前缀虽像 Ogg, 里面却不是能解的流(截断 / 坏页 / 是 Opus 等)
            m_file.close();
            AUDIO_LOG_ERROR(kTag, "ov_open 失败(不是可解的 Vorbis 流): %s", utf8Path);
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        }
        m_open = true;                  // 从这一行起才欠 ov_clear 一次调用

        // ---- 头信息: 声道数 / 采样率   播放器拿它填 WAVEFORMATEX ----
        vorbis_info* const vi = ov_info(&m_vf, -1);
        if (!vi) {
            Close();
            return AudioSdk::AudioSdkState::FORMAT_NOT_SUPPORTED;
        }
        m_channels = vi->channels;
        m_rate     = vi->rate;
        // 统一按 16bit 解码(见 Read 里 ov_read 的参数), 所以每帧就是声道数×2
        m_blockAlign = static_cast<uint16_t>(m_channels * (BITS_PER_SAMPLE / 8));

        // ---- PCM 总长: 给进度条当分母 ----
        const ogg_int64_t totalSamples = ov_pcm_total(&m_vf, -1);
        if (totalSamples < 0) {
            Close();
            return AudioSdk::AudioSdkState::FILE_READ_FAILED;
        }
        m_totalBytes = static_cast<size_t>(totalSamples) * m_blockAlign;
    }
    catch (const std::filesystem::filesystem_error&) {
        Close();
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;
    }
    catch (const std::exception&) {
        Close();
        return AudioSdk::AudioSdkState::UNKNOWN_ERROR;
    }

    return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 读下一段 PCM
 * @param dst 目标缓冲区, 至少 n 字节大
 * @param n 要读的字节数
 * @return 实际读到的字节数: = n 正常; < n 到尾了; 0 已经读干净
 */
size_t COggStreamReader::Read(uint8_t* dst, size_t n){
    if (!m_open || !dst)
        return 0;

    size_t done = 0;
    while (done < n) {
        const size_t left = n - done;
        // ov_read 的长度参数是 int, 一次要 1GB 以上会溢出 —— 卡住上限即可
        const int iAsk = static_cast<int>(left > (1u << 30) ? (1u << 30) : left);

        int iSection = 0;                // 输出参数: 这一段属于第几条链(我们只有一条)
        // 参数: (句柄, 目标缓冲, 最多这么多字节, 大端?否, 每采样 2 字节, 有符号?, 输出链号)
        const long lGot = ov_read(&m_vf, reinterpret_cast<char*>(dst + done), iAsk,
                                 0, 2, 1, &iSection);
        if (lGot > 0) {
            done += static_cast<size_t>(lGot);
            continue;
        }
        if (lGot == 0)
            break;                      // 正常读到底

        // lGot < 0: 页 CRC 对不上或数据有缺口
        AUDIO_LOG_ERROR(kTag, "解码中断: ov_read 返回 %ld", lGot);
        break;
    }
    return done;
}

/**
 * @brief 按 PCM 字节偏移定位(播放器的 Seek 用)
 * @param pcmBytes 目标位置, 相对整条流开头
 * @return AudioSdk::AudioSdkState 成功或失败
 */
AudioSdk::AudioSdkState COggStreamReader::SeekTo(size_t pcmBytes){
    if (!m_open)
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;

    const ogg_int64_t sample = static_cast<ogg_int64_t>(pcmBytes / m_blockAlign);
    const int iRet = ov_pcm_seek(&m_vf, sample);
    if (iRet != 0) {
        AUDIO_LOG_ERROR(kTag, "定位失败: 第 %lld 个采样 -> %d", (long long)sample, iRet);
        return AudioSdk::AudioSdkState::FILE_READ_FAILED;
    }
    return AudioSdk::AudioSdkState::NONE;
}

/**
 * @brief 整条解一遍, 把 PCM 降采样成 points 个峰值点
 * @param out      输出缓冲, 装得下 points*2 个 float
 * @param points   峰值点个数
 * @param cb       进度回调(可传 NULL); 每次给的 points 参数是【已填了几个点】
 * @param userData 原样透传给回调
 * @return 解完返回 NONE; 没打开过返回 FILE_OPEN_FAILED
 */
AudioSdk::AudioSdkState COggStreamReader::BuildPeaks(float* out, int points,
                                                     AudioSdkWaveCallback cb, void* userData){
    if (!m_open || !out || points <= 0)
        return AudioSdk::AudioSdkState::FILE_OPEN_FAILED;

    const size_t nPoints = static_cast<size_t>(points);
    for (size_t i = 0; i < nPoints * 2; ++i) out[i] = 0.0f;

    // 每个点覆盖多少字节
    size_t samples = m_totalBytes / sizeof(int16_t) / nPoints;
    if (samples == 0) samples = 1;
    const size_t stepBytes = samples * sizeof(int16_t);

    // 每积够这么多点回报一次
    const size_t reportEvery = nPoints > 32 ? nPoints / 32 : 1;

    std::vector<uint8_t> buf(64 * 1024);
    size_t consumed = 0;        // 已解出的 PCM 字节数
    size_t filled   = 0;        // 已有几个点算出来了
    size_t reported = 0;        // 上次回调报到第几个点

    for (size_t i = 0; i < nPoints && consumed < m_totalBytes; ++i){
        // 最后一个点吃掉剩余全部, 否则总长不是 points 整数倍时尾部那段永远画不出来
        const size_t end = (i + 1 == nPoints)
                               ? m_totalBytes
                               : (std::min)((i + 1) * stepBytes, m_totalBytes);

        float mn = 0.0f, mx = 0.0f;
        bool  any = false;
        while (consumed < end){
            const size_t want = (std::min)(buf.size(), end - consumed);
            const size_t got  = Read(buf.data(), want);   // 要多少给多少, 除非流到底
            if (got == 0) break;                          // 截断: 剩下的点保持 0

            // 这一整段都属于第 i 个点, 所以降成 1 个点再跟前面几次的并起来
            float mm[2] = {};
            CWaveform::ComputePeaks(buf.data(), got, mm, 1);
            if (!any || mm[0] < mn) mn = mm[0];
            if (!any || mm[1] > mx) mx = mm[1];
            any = true;
            consumed += got;
        }

        if (any){
            out[i * 2]     = mn;
            out[i * 2 + 1] = mx;
            filled = i + 1;
        }

        // 报的是"已填了几个点", 不是 points —— 调用方照着这个数只画左边这一截
        if (cb && filled - reported >= reportEvery){
            reported = filled;
            cb(out, static_cast<int>(filled), userData);
        }
    }

    // 收个尾: 循环里最后那次回调不一定正好落在末尾(或文件被截断, filled < points)
    if (cb && reported != filled)
        cb(out, static_cast<int>(filled), userData);
    return AudioSdk::AudioSdkState::NONE;
}
