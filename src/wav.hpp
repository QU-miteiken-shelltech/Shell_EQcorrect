// wav.hpp - WAV (RIFF) 読み書き。内部表現は double [-1,1) のチャンネル別配列
// 対応入力: PCM 8/16/24/32bit, IEEE float 32/64bit, WAVE_FORMAT_EXTENSIBLE
// 非対応: RF64/BW64 (4GB超)、圧縮形式
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "platform.hpp"

namespace eq {

struct WavData {
    int fs = 0;
    int channels = 0;
    int bits = 0;          // 元ファイルのビット深度
    bool is_float = false; // 元ファイルが float か
    uint32_t channel_mask = 0;  // WAVE_FORMAT_EXTENSIBLE の dwChannelMask (無ければ 0)
    std::vector<std::vector<double>> ch;  // [channel][frame]
    size_t frames() const { return ch.empty() ? 0 : ch[0].size(); }
};

struct WriteStats {
    size_t clipped = 0;     // クリップしたサンプル数
    double peak = 0.0;      // 書き込み前のピーク (線形, 絶対値)
};

namespace detail {
inline uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline int popcount32(uint32_t x) { int n = 0; while (x) { n += (int)(x & 1u); x >>= 1; } return n; }
inline void wr16(std::vector<unsigned char>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
inline void wr32(std::vector<unsigned char>& v, uint32_t x) {
    for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 0xFF);
}
}  // namespace detail

inline bool read_wav(const std::string& path, WavData& w, std::string& err) {
    using namespace detail;
    std::vector<unsigned char> buf;
    if (!read_file(path, buf, err)) return false;
    if (buf.size() < 12 || std::memcmp(buf.data(), "RIFF", 4) != 0 || std::memcmp(buf.data() + 8, "WAVE", 4) != 0) {
        if (buf.size() >= 4 && (std::memcmp(buf.data(), "RF64", 4) == 0 || std::memcmp(buf.data(), "BW64", 4) == 0))
            err = "RF64/BW64 形式 (4GB超) には対応していません";
        else
            err = "RIFF/WAVE 形式ではありません: " + path;
        return false;
    }
    size_t pos = 12;
    bool have_fmt = false;
    uint16_t fmt_tag = 0, nch = 0, bits = 0, block_align = 0;
    uint32_t fs = 0, ch_mask = 0;
    const unsigned char* data = nullptr;
    size_t data_len = 0;
    while (pos + 8 <= buf.size()) {
        const unsigned char* ck = buf.data() + pos;
        uint32_t sz = rd32(ck + 4);
        size_t body = pos + 8;
        size_t avail = buf.size() - body;
        if (std::memcmp(ck, "fmt ", 4) == 0 && sz >= 16 && avail >= 16) {
            const unsigned char* p = buf.data() + body;
            fmt_tag = rd16(p); nch = rd16(p + 2); fs = rd32(p + 4);
            block_align = rd16(p + 12); bits = rd16(p + 14);
            if (fmt_tag == 0xFFFE && sz >= 40 && avail >= 40) {
                ch_mask = rd32(p + 20);          // dwChannelMask
                fmt_tag = rd16(p + 24);          // SubFormat GUID 先頭2byte
            }
            have_fmt = true;
        } else if (std::memcmp(ck, "data", 4) == 0) {
            data = buf.data() + body;
            // ストリーミング出力等で size が 0 / 0xFFFFFFFF の場合は残り全部を使う
            data_len = (sz == 0 || sz == 0xFFFFFFFFu || sz > avail) ? avail : sz;
            break;  // data が見つかれば十分 (後続チャンクは不要)
        }
        size_t adv = (size_t)sz + (sz & 1);
        if (adv > avail) break;
        pos = body + adv;
    }
    if (!have_fmt) { err = "fmt チャンクが見つかりません"; return false; }
    if (!data) { err = "data チャンクが見つかりません"; return false; }
    if (nch < 1) { err = "チャンネル数が不正です"; return false; }
    if (fs < 1 || fs > 10000000u) { err = "サンプリング周波数が不正です: " + std::to_string(fs); return false; }
    bool isf = (fmt_tag == 3);
    if (!(fmt_tag == 1 || fmt_tag == 3)) { err = "未対応のWAVフォーマットタグ: " + std::to_string(fmt_tag); return false; }
    if (isf && !(bits == 32 || bits == 64)) { err = "未対応のfloatビット深度"; return false; }
    if (!isf && !(bits == 8 || bits == 16 || bits == 24 || bits == 32)) {
        err = "未対応のPCMビット深度: " + std::to_string(bits); return false;
    }
    size_t bytes_per = bits / 8;
    if (nch > 1024) { err = "チャンネル数が多すぎます (最大 1024): " + std::to_string(nch); return false; }
    // 1フレームのバイト数はヘッダの block_align を信用せず、チャンネル数とビット深度から求める (壊れたヘッダに寛容)
    const size_t frame_bytes = (size_t)nch * bytes_per;
    (void)block_align;
    size_t frames = data_len / frame_bytes;
    w.fs = (int)fs; w.channels = nch; w.bits = bits; w.is_float = isf; w.channel_mask = ch_mask;
    w.ch.assign(nch, std::vector<double>(frames));
    for (size_t i = 0; i < frames; i++) {
        const unsigned char* fp = data + i * frame_bytes;
        for (int c = 0; c < nch; c++) {
            const unsigned char* p = fp + c * bytes_per;
            double v;
            if (isf) {
                if (bits == 32) { float f; std::memcpy(&f, p, 4); v = f; }
                else { double d; std::memcpy(&d, p, 8); v = d; }
            } else if (bits == 8) {
                v = ((int)p[0] - 128) / 128.0;
            } else if (bits == 16) {
                v = (int16_t)rd16(p) / 32768.0;
            } else if (bits == 24) {
                int32_t x = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8;
                v = x / 8388608.0;
            } else {
                v = (int32_t)rd32(p) / 2147483648.0;
            }
            if (!std::isfinite(v)) v = 0.0;
            w.ch[c][i] = v;
        }
    }
    return true;
}

// out_bits: 16/24/32 (PCM) または 0 = 32bit float。dither は 16bit PCM 出力時のみ有効 (TPDF ±1LSB)
inline bool write_wav(const std::string& path, const WavData& w, int out_bits, bool out_float,
                      bool dither, WriteStats& st, std::string& err) {
    using namespace detail;
    const int nch = w.channels;
    const size_t frames = w.frames();
    const int bits = out_float ? 32 : out_bits;
    const size_t bps = bits / 8;
    const bool ext = (nch > 2) || (bits > 16);
    const size_t data_bytes = frames * nch * bps;
    if (data_bytes + 100 > 0xFFFFFFFFull) { err = "出力が4GBを超えます (RF64 非対応)"; return false; }

    std::vector<unsigned char> hdr;
    hdr.reserve(80);
    auto tag = [&](const char* t) { for (int i = 0; i < 4; i++) hdr.push_back((unsigned char)t[i]); };
    tag("RIFF"); wr32(hdr, (uint32_t)(4 + (8 + (ext ? 40 : 16)) + 8 + data_bytes + (data_bytes & 1)));
    tag("WAVE");
    tag("fmt "); wr32(hdr, ext ? 40 : 16);
    wr16(hdr, ext ? 0xFFFE : (out_float ? 3 : 1));
    wr16(hdr, (uint16_t)nch);
    wr32(hdr, (uint32_t)w.fs);
    wr32(hdr, (uint32_t)((uint64_t)w.fs * (uint64_t)nch * (uint64_t)bps));
    wr16(hdr, (uint16_t)(nch * bps));
    wr16(hdr, (uint16_t)bits);
    if (ext) {
        wr16(hdr, 22);
        wr16(hdr, (uint16_t)bits);
        // 入力のチャンネルマスクを保持 (ビット数がチャンネル数と一致する場合のみ)。無ければ標準的な配置
        uint32_t mask = (nch == 1) ? 0x4 : (nch == 2) ? 0x3 : (nch == 4) ? 0x33 : (nch == 6) ? 0x3F : (nch == 8) ? 0x63F : 0;
        if (w.channel_mask != 0 && popcount32(w.channel_mask) == nch) mask = w.channel_mask;
        wr32(hdr, mask);
        wr16(hdr, out_float ? 3 : 1);  // SubFormat GUID: {format, 0000-0010-8000-00AA00389B71}
        static const unsigned char guid_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        for (int i = 0; i < 14; i++) hdr.push_back(guid_tail[i]);
    }
    tag("data"); wr32(hdr, (uint32_t)data_bytes);

    FILE* f = fopen_u8(path, "wb");
    if (!f) { err = "出力ファイルを開けません: " + path; return false; }
    bool ok = std::fwrite(hdr.data(), 1, hdr.size(), f) == hdr.size();

    uint64_t rng = 0x9E3779B97F4A7C15ull;
    auto rnd = [&]() {  // xorshift64* → [0,1)
        rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27;
        return (double)((rng * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
    };

    st = WriteStats();
    const size_t chunk = 1 << 16;
    std::vector<unsigned char> blk(chunk * nch * bps);
    for (size_t base = 0; base < frames && ok; base += chunk) {
        size_t cnt = std::min(chunk, frames - base);
        unsigned char* q = blk.data();
        for (size_t i = 0; i < cnt; i++) {
            for (int c = 0; c < nch; c++) {
                double v = w.ch[c][base + i];
                st.peak = std::max(st.peak, std::fabs(v));
                if (out_float) {
                    float fv = (float)v;
                    if (std::fabs(v) > 1.0) st.clipped++;  // float は実際にはクリップしないが、0dBFS超過として数える
                    std::memcpy(q, &fv, 4); q += 4;
                    continue;
                }
                const double scale = (double)((int64_t)1 << (bits - 1));
                double x = v * scale;
                if (dither && bits == 16) x += rnd() - rnd();  // TPDF
                double r = std::nearbyint(x);
                const double lo = -scale, hi = scale - 1.0;
                if (r > hi) { r = hi; st.clipped++; }
                else if (r < lo) { r = lo; st.clipped++; }
                int32_t iv = (int32_t)r;
                if (bits == 16) { q[0] = iv & 0xFF; q[1] = (iv >> 8) & 0xFF; q += 2; }
                else if (bits == 24) { q[0] = iv & 0xFF; q[1] = (iv >> 8) & 0xFF; q[2] = (iv >> 16) & 0xFF; q += 3; }
                else { q[0] = iv & 0xFF; q[1] = (iv >> 8) & 0xFF; q[2] = (iv >> 16) & 0xFF; q[3] = (iv >> 24) & 0xFF; q += 4; }
            }
        }
        ok = std::fwrite(blk.data(), 1, (size_t)(q - blk.data()), f) == (size_t)(q - blk.data());
    }
    if (ok && (data_bytes & 1)) { unsigned char z = 0; ok = std::fwrite(&z, 1, 1, f) == 1; }
    if (std::fclose(f) != 0) ok = false;
    if (!ok) { err = "書き込みエラー: " + path; return false; }
    return true;
}

}  // namespace eq
