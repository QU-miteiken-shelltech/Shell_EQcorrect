// eqcorrect - 周波数特性(振幅・位相)補正エンジン  (オフライン高品質処理)
//
//   入力: .wav (リニアPCM / float) + .req (測定データ)
//   出力: 補正済み .wav + 可視化用 JSON (+ file:// で読める JS 版)
//
// ビルド: Makefile / build_msvc.bat / build_mingw.bat を参照 (単一翻訳単位, 外部ライブラリ不要)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "analysis.hpp"
#include "design.hpp"
#include "jsonw.hpp"
#include "platform.hpp"
#include "req.hpp"
#include "wav.hpp"

using namespace eq;

static const char* kVersion = "1.0.0";

static void usage() {
    std::puts(
"eqcorrect " "1.0.0" " - 周波数特性 (振幅・位相) 補正エンジン\n"
"\n"
"使い方:\n"
"  eqcorrect <入力.wav> <測定.req> [-o 出力.wav] [-j 解析.json] [オプション]\n"
"\n"
"入出力:\n"
"  -o, --out FILE        出力WAV (既定: <入力>_corrected.wav)\n"
"  -j, --json FILE       解析JSON (既定: 出力WAVと同じ場所の result.json)。同名の .js も出力します\n"
"      --dump-req        .req の構造を表示して終了 (形式調査用)   例: eqcorrect --dump-req 測定.req\n"
"\n"
".req の読み込み:\n"
"      --req-format F    auto | java | text | ir-text | raw-f32 | raw-f64   (既定 auto)\n"
"      --req-fs HZ       インパルス応答のサンプリング周波数 (既定: ファイル内の値 → なければWAVと同じ)\n"
"      --req-mag-unit U  db | linear      (既定 db)\n"
"      --req-phase-unit U deg | rad       (既定 deg)\n"
"      --java-map MAP    Java形式での割り当て指定  例: freq=Eq.freq,mag=Eq.gain,phase=Eq.phase  (または ir=パス)\n"
"      --req-role R      REW のEQフィルタ(.req)の扱い: auto(=correction) | correction(そのEQを補正として適用) |\n"
"                        measurement(その合成特性を測定とみなし、逆特性を作る)\n"
"      --dump-full       --dump-req で配列を全要素表示 (REW のフィルタ 28 スロットの確認など)\n"
"\n"
"フィルタ設計:\n"
"      --taps N          補正フィルタ長 = FFTサイズ (2のべき乗, 既定 65536)\n"
"      --phase M         full(測定位相を逆に; 既定) | min(最小位相) | linear(位相補正なし)\n"
"      --max-boost DB    ブースト上限 (既定 12)\n"
"      --max-cut DB      カット上限 (既定 24)\n"
"      --ref-band LO,HI  基準レベルを平均する帯域[Hz] (既定 500,2000)\n"
"      --no-normalize    基準レベルの正規化をしない (測定値の絶対値をそのまま逆にする)\n"
"      --smooth N        測定を 1/N オクターブ平滑化してから逆特性化 (既定 0=なし)\n"
"      --edge-oct X      測定範囲端で補正量を 0 に収束させる幅[oct] (既定 0.5, 0で無効)\n"
"      --fmin HZ --fmax HZ  補正する帯域を制限 (既定: 測定範囲)\n"
"      --window W        tukey(既定) | bh | hann | none  (補正IRの窓)\n"
"      --remove-delay    測定位相の純遅延成分(直線位相)を補正対象から除く\n"
"      --save-filter F   設計した補正フィルタを float32 モノラルWAVで保存 (付加遅延 taps/2 を含む。他のコンボルバ用/検証用)\n"
"\n"
"出力:\n"
"      --bits B          same(既定) | 16 | 24 | 32 | f32\n"
"      --dither D        auto(16bit出力時のみTPDF; 既定) | on | off\n"
"      --gain-db G       出力ゲイン[dB] (既定 0)\n"
"      --protect-clip    0dBFS を超える場合に -0.1dBFS まで自動で下げる (既定: 下げず警告のみ)\n"
"  -h, --help            このヘルプ\n");
}

struct Cli {
    std::string wav, req, out, json;
    bool dump_req = false;
    ReqOptions ro;
    DesignOptions dop;
    std::string bits = "same", dither = "auto", save_filter;
    double gain_db = 0;
    bool protect_clip = false;
};

static bool parse_double(const std::string& s, double& v) {
    char* e = nullptr; v = std::strtod(s.c_str(), &e);
    return e && *e == '\0' && !s.empty() && std::isfinite(v);
}

static bool parse_cli(const std::vector<std::string>& a, Cli& c, std::string& err) {
    std::vector<std::string> pos;
    for (size_t i = 1; i < a.size(); i++) {
        const std::string& s = a[i];
        auto next = [&](std::string& dst) -> bool {
            if (i + 1 >= a.size()) { err = s + " には値が必要です"; return false; }
            dst = a[++i]; return true;
        };
        auto next_num = [&](double& dst) -> bool {
            std::string t; if (!next(t)) return false;
            if (!parse_double(t, dst)) { err = s + " の値が数値ではありません: " + t; return false; }
            return true;
        };
        std::string t; double d;
        if (s == "-h" || s == "--help") { usage(); std::exit(0); }
        else if (s == "-o" || s == "--out") { if (!next(c.out)) return false; }
        else if (s == "-j" || s == "--json") { if (!next(c.json)) return false; }
        else if (s == "--wav") { if (!next(c.wav)) return false; }
        else if (s == "--req") { if (!next(c.req)) return false; }
        else if (s == "--dump-req") c.dump_req = true;
        else if (s == "--dump-full") { c.dump_req = true; jser::dump_limit() = 100000; }
        else if (s == "--req-format") { if (!next(c.ro.format)) return false; }
        else if (s == "--req-fs") { if (!next_num(c.ro.fs)) return false; }
        else if (s == "--req-mag-unit") { if (!next(c.ro.mag_unit)) return false; if (c.ro.mag_unit != "db" && c.ro.mag_unit != "linear") { err = "--req-mag-unit は db か linear"; return false; } }
        else if (s == "--req-phase-unit") { if (!next(c.ro.phase_unit)) return false; if (c.ro.phase_unit != "deg" && c.ro.phase_unit != "rad") { err = "--req-phase-unit は deg か rad"; return false; } }
        else if (s == "--java-map") { if (!next(c.ro.java_map)) return false; }
        else if (s == "--req-role") { if (!next(c.dop.req_role)) return false; if (c.dop.req_role != "auto" && c.dop.req_role != "correction" && c.dop.req_role != "measurement") { err = "--req-role は auto|correction|measurement"; return false; } }
        else if (s == "--taps") { if (!next_num(d)) return false; c.dop.taps = (size_t)d; }
        else if (s == "--phase") { if (!next(c.dop.phase_mode)) return false; }
        else if (s == "--max-boost") { if (!next_num(c.dop.max_boost_db)) return false; }
        else if (s == "--max-cut") { if (!next_num(c.dop.max_cut_db)) return false; }
        else if (s == "--ref-band") {
            if (!next(t)) return false;
            size_t q = t.find(',');
            if (q == std::string::npos || !parse_double(t.substr(0, q), c.dop.ref_lo) || !parse_double(t.substr(q + 1), c.dop.ref_hi) || c.dop.ref_hi <= c.dop.ref_lo) {
                err = "--ref-band は LO,HI (例: 500,2000) の形式で指定してください"; return false;
            }
        }
        else if (s == "--no-normalize") c.dop.normalize = false;
        else if (s == "--smooth") { if (!next_num(d)) return false; c.dop.smooth = (int)d; }
        else if (s == "--edge-oct") { if (!next_num(c.dop.edge_oct)) return false; }
        else if (s == "--fmin") { if (!next_num(c.dop.fmin)) return false; }
        else if (s == "--fmax") { if (!next_num(c.dop.fmax)) return false; }
        else if (s == "--window") { if (!next(c.dop.window)) return false; if (c.dop.window != "tukey" && c.dop.window != "bh" && c.dop.window != "hann" && c.dop.window != "none") { err = "--window は tukey|bh|hann|none"; return false; } }
        else if (s == "--remove-delay") c.dop.remove_delay = true;
        else if (s == "--save-filter") { if (!next(c.save_filter)) return false; }
        else if (s == "--bits") { if (!next(c.bits)) return false; }
        else if (s == "--dither") { if (!next(c.dither)) return false; }
        else if (s == "--gain-db") { if (!next_num(c.gain_db)) return false; }
        else if (s == "--protect-clip") c.protect_clip = true;
        else if (!s.empty() && s[0] == '-' && s.size() > 1) { err = "未知のオプション: " + s + " (--help で一覧)"; return false; }
        else pos.push_back(s);
    }
    if (c.dump_req && pos.size() == 1 && c.req.empty()) { c.req = pos[0]; pos.clear(); }  // --dump-req は .req だけで使える
    if (pos.size() > 0 && c.wav.empty()) c.wav = pos[0];
    if (pos.size() > 1 && c.req.empty()) c.req = pos[1];
    if (pos.size() > 2) { err = "余分な引数があります: " + pos[2]; return false; }
    if (c.dop.phase_mode != "full" && c.dop.phase_mode != "min" && c.dop.phase_mode != "linear") { err = "--phase は full|min|linear"; return false; }
    if (c.bits != "same" && c.bits != "16" && c.bits != "24" && c.bits != "32" && c.bits != "f32") { err = "--bits は same|16|24|32|f32"; return false; }
    if (c.dither != "auto" && c.dither != "on" && c.dither != "off") { err = "--dither は auto|on|off"; return false; }
    if (c.dop.smooth < 0) { err = "--smooth は 0 以上"; return false; }
    if (c.req.empty()) { err = "測定ファイル (.req) が指定されていません"; return false; }
    if (c.wav.empty() && !c.dump_req) { err = "入力WAVが指定されていません"; return false; }
    return true;
}

static std::string dir_of(const std::string& p) {
    size_t i = p.find_last_of("/\\");
    return i == std::string::npos ? "" : p.substr(0, i + 1);
}
static std::string strip_ext(const std::string& p) {
    size_t s = p.find_last_of("/\\"), d = p.find_last_of('.');
    return (d == std::string::npos || (s != std::string::npos && d < s)) ? p : p.substr(0, d);
}

static bool write_text(const std::string& path, const std::string& s) {
    FILE* f = fopen_u8(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
    return std::fclose(f) == 0 && ok;
}

int main(int argc, char** argv) {
    console_utf8();
    std::vector<std::string> args = get_args_utf8(argc, argv);
    if (args.size() < 2) { usage(); return 1; }
    Cli cli; std::string err;
    if (!parse_cli(args, cli, err)) { std::fprintf(stderr, "エラー: %s\n", err.c_str()); return 1; }

    std::vector<std::string> log;
    auto say = [&](const std::string& s) { std::printf("%s\n", s.c_str()); std::fflush(stdout); log.push_back(s); };
    Timer total;

    // ---- .req 読み込み ----
    ReqResult rq;
    if (!load_req(cli.req, cli.ro, rq, err)) {
        std::fprintf(stderr, "エラー (.req): %s\n", err.c_str());
        if (!rq.dump.empty() && cli.dump_req) std::printf("--- 構造ダンプ ---\n%s", rq.dump.c_str());
        return 2;
    }
    if (cli.dump_req) {
        std::printf("形式判別: %s\n", rq.detected.c_str());
        for (auto& l : rq.log) std::printf("  %s\n", l.c_str());
        if (!rq.dump.empty()) std::printf("--- 構造ダンプ (Java シリアライズ) ---\n%s", rq.dump.c_str());
        else if (!rq.is_ir) {
            std::printf("--- 先頭 5 点 ---\n");
            for (size_t i = 0; i < rq.fr.f.size() && i < 5; i++)
                std::printf("  %.6g Hz  %.6g dB  %s\n", rq.fr.f[i], rq.fr.mag_db[i], rq.fr.has_phase ? (std::to_string(rq.fr.phase_deg[i]) + " deg").c_str() : "(位相なし)");
        }
        return 0;
    }
    say(std::string("[.req] ") + rq.detected);
    for (auto& l : rq.log) say("[.req] " + l);

    // ---- WAV 読み込み ----
    WavData wav;
    if (!read_wav(cli.wav, wav, err)) { std::fprintf(stderr, "エラー (WAV): %s\n", err.c_str()); return 3; }
    const size_t frames = wav.frames();
    char b[256];
    std::snprintf(b, sizeof b, "[WAV] %d Hz, %d ch, %s%d bit, %zu frames (%.2f 秒)", wav.fs, wav.channels, wav.is_float ? "float " : "PCM ", wav.bits, frames, (double)frames / wav.fs);
    say(b);
    if (frames == 0) { std::fprintf(stderr, "エラー: WAV にサンプルがありません\n"); return 3; }
    if (rq.fs_hint > 0 && !rq.is_ir && std::fabs(rq.fs_hint - wav.fs) > 1) say("[.req] 注意: .req 内のサンプリング周波数 (" + std::to_string((int)rq.fs_hint) + "Hz) が WAV と異なります (周波数特性は Hz 単位なのでそのまま使用)");
    if (rq.is_ir && cli.ro.fs > 0) rq.fs_hint = cli.ro.fs;

    // ---- 逆フィルタ設計 ----
    Timer t_design;
    DesignResult ds;
    if (!design_filter(rq, wav.fs, cli.dop, ds, err)) { std::fprintf(stderr, "エラー (設計): %s\n", err.c_str()); return 4; }
    for (auto& l : ds.log) say("[設計] " + l);
    std::snprintf(b, sizeof b, "[設計] フィルタ長 %zu taps, 付加遅延 %zu サンプル (%.2f ms; 出力では打ち消し済み), 位相方式=%s, 基準レベル=%.2f dB, 補正帯域 %.1f〜%.1f Hz, 窓=%s",
                  ds.c.size(), ds.delay, 1000.0 * (double)ds.delay / wav.fs, ds.phase_used.c_str(), ds.ref_level_db, ds.fmin_used, ds.fmax_used, cli.dop.window.c_str());
    say(b);
    double design_sec = t_design.sec();
    if (!cli.save_filter.empty()) {
        WavData fw; fw.fs = wav.fs; fw.channels = 1; fw.bits = 32; fw.is_float = true; fw.ch.push_back(ds.c);
        WriteStats fws;
        if (!write_wav(cli.save_filter, fw, 32, true, false, fws, err)) { std::fprintf(stderr, "エラー (フィルタ保存): %s\n", err.c_str()); return 5; }
        say("[設計] 補正フィルタを保存: " + cli.save_filter + " (float32, 付加遅延 " + std::to_string(ds.delay) + " サンプル)");
    }

    // ---- 畳み込み (FFT / オーバーラップ加算) ----
    Timer t_conv;
    WavData outw = wav;
    {
        Convolver cv(ds.c, ds.delay, frames);
        std::snprintf(b, sizeof b, "[畳み込み] FFTサイズ %zu, ブロック %zu サンプル, %s", cv.fft_size(), cv.block(),
#if defined(__AVX2__)
                      "AVX2 経路");
#else
                      "スカラー経路 (AVX2 なし)");
#endif
        say(b);
        for (int ch = 0; ch < wav.channels; ch += 2) {
            const std::vector<double>* x2 = (ch + 1 < wav.channels) ? &wav.ch[ch + 1] : nullptr;
            cv.process(wav.ch[ch], x2, outw.ch[ch], x2 ? &outw.ch[ch + 1] : nullptr);
            std::fprintf(stderr, "\r  処理中: ch %d/%d 完了", std::min(ch + 2, wav.channels), wav.channels);
        }
        std::fprintf(stderr, "\n");
    }
    double conv_sec = t_conv.sec();

    // ---- ゲイン / クリップ保護 ----
    double gain = std::pow(10.0, cli.gain_db / 20.0);
    double pk = 0; for (auto& c : outw.ch) pk = std::max(pk, peak_of(c));
    if (cli.gain_db != 0) say("[出力] ゲイン " + std::to_string(cli.gain_db) + " dB を適用");
    double pk_after = pk * gain;
    if (cli.protect_clip && pk_after > 0.9886) {   // -0.1 dBFS
        double g2 = 0.9886 / pk_after;
        gain *= g2;
        std::snprintf(b, sizeof b, "[出力] ピークが 0dBFS を超えるため %.2f dB 下げました (--protect-clip)", to_db(g2));
        say(b);
        pk_after = 0.9886;
    }
    if (gain != 1.0) for (auto& c : outw.ch) for (auto& v : c) v *= gain;

    // ---- 出力フォーマット ----
    int ob = 16; bool of = false;
    if (cli.bits == "same") {
        if (wav.is_float) { of = true; ob = 32; }
        else ob = (wav.bits == 8) ? 16 : wav.bits;
    } else if (cli.bits == "f32") { of = true; ob = 32; }
    else ob = std::atoi(cli.bits.c_str());
    bool dither = (cli.dither == "on") || (cli.dither == "auto" && !of && ob == 16);
    if (cli.bits == "same" && wav.is_float && wav.bits == 64) say("[出力] 入力が float64 のため float32 で出力します");
    if (cli.bits == "same" && !wav.is_float && wav.bits == 8) say("[出力] 入力が 8bit のため 16bit で出力します");
    if (cli.out.empty()) cli.out = strip_ext(cli.wav) + "_corrected.wav";
    WriteStats ws;
    if (!write_wav(cli.out, outw, ob, of, dither, ws, err)) { std::fprintf(stderr, "エラー (出力): %s\n", err.c_str()); return 5; }
    std::snprintf(b, sizeof b, "[出力] %s  (%s%d bit%s, ピーク %.2f dBFS)", cli.out.c_str(), of ? "float " : "PCM ", ob, dither ? ", TPDFディザ" : "", to_db(ws.peak));
    say(b);
    if (ws.clipped > 0 && !of) {
        std::snprintf(b, sizeof b, "[警告] %zu サンプルがクリップしました。--gain-db で下げるか --protect-clip を使ってください", ws.clipped);
        say(b);
    } else if (ws.clipped > 0) {
        say("[注意] float 出力のため値は保持されていますが、0dBFS を超えるサンプルが " + std::to_string(ws.clipped) + " 個あります");
    }

    // ---- 解析データ ----
    Timer t_an;
    JsonWriter jw;
    jw.begin_obj();
    jw.key("meta"); jw.begin_obj();
    jw.kv("version", kVersion);
    jw.kv("input_wav", basename_of(cli.wav)); jw.kv("output_wav", basename_of(cli.out)); jw.kv("req_file", basename_of(cli.req));
    jw.kv("fs", wav.fs); jw.kv("channels", wav.channels); jw.kv("frames", frames); jw.kv("duration_sec", (double)frames / wav.fs);
    jw.kv("req_detected", rq.detected);
    jw.kv("req_has_phase", ds.meas_has_phase);
    jw.kv("phase_mode_requested", cli.dop.phase_mode); jw.kv("phase_mode_used", ds.phase_used);
    jw.kv("req_role", ds.role);
    jw.key("req_filters"); jw.begin_arr();
    for (auto& f : rq.filters) { jw.begin_obj(); jw.kv("type", f.type); jw.kv("fc", f.fc); jw.kv("gain", f.gain); jw.kv("q", f.q); jw.end_obj(); }
    jw.end_arr();
    jw.kv("taps", ds.c.size()); jw.kv("delay_samples", ds.delay); jw.kv("ref_level_db", ds.ref_level_db);
    jw.kv("fmin_used", ds.fmin_used); jw.kv("fmax_used", ds.fmax_used);
    jw.kv("max_boost_db", cli.dop.max_boost_db); jw.kv("max_cut_db", cli.dop.max_cut_db);
    jw.kv("smooth_oct", cli.dop.smooth); jw.kv("edge_oct", cli.dop.edge_oct); jw.kv("window", cli.dop.window);
    jw.kv("normalize", cli.dop.normalize); jw.kv("remove_delay", cli.dop.remove_delay); jw.kv("removed_delay_ms", ds.removed_delay_ms);
    jw.kv("out_bits", ob); jw.kv("out_float", of); jw.kv("dither", dither); jw.kv("gain_db", cli.gain_db);
    jw.kv("clipped_samples", ws.clipped);
#if defined(__AVX2__)
    jw.kv("simd", "AVX2");
#else
    jw.kv("simd", "scalar");
#endif
    jw.end_obj();

    // 周波数応答 (対数グリッド)
    jw.arr("grid", ds.grid_f);
    jw.key("response"); jw.begin_obj();
    jw.arr("measured_db", ds.meas_db);
    if (ds.meas_has_phase) jw.arr("measured_phase_deg", ds.meas_ph);
    jw.arr("correction_target_db", ds.tgt_db);
    jw.arr("correction_target_phase_deg", ds.tgt_ph);
    jw.arr("correction_actual_db", ds.act_db);
    jw.arr("correction_actual_phase_deg", ds.act_ph);
    jw.arr("residual_db", ds.res_db);
    if (ds.meas_has_phase) jw.arr("residual_phase_deg", ds.res_ph);
    jw.end_obj();

    // スペクトル (補正前後) と実測伝達関数
    const int nch_out = std::min(wav.channels, 8);
    jw.key("spectrum"); jw.begin_arr();
    for (int ch = 0; ch < nch_out; ch++) {
        SpectrumOut so;
        welch_channel(wav.ch[ch], outw.ch[ch], wav.fs, ds.grid_f, ds.ppo, so);
        jw.begin_obj();
        jw.kv("channel", ch + 1);
        jw.arr("in_db", so.in_db); jw.arr("out_db", so.out_db);
        jw.arr("h_db", so.h_db); jw.arr("h_phase_deg", so.h_ph);
        jw.end_obj();
    }
    jw.end_arr();

    // 波形エンベロープ
    jw.key("waveform"); jw.begin_arr();
    for (int ch = 0; ch < nch_out; ch++) {
        Envelope e; make_envelope(wav.ch[ch], outw.ch[ch], wav.fs, 1600, e);
        jw.begin_obj();
        jw.kv("channel", ch + 1);
        jw.arr("t", e.t); jw.arr("in_min", e.in_min); jw.arr("in_max", e.in_max); jw.arr("out_min", e.out_min); jw.arr("out_max", e.out_max);
        jw.end_obj();
    }
    jw.end_arr();

    // 補正フィルタのインパルス応答 (中心 ±4096 サンプル)
    {
        const size_t half = std::min<size_t>(4096, ds.c.size() / 2);
        std::vector<double> tms, cv;
        for (size_t n = ds.delay - half; n < ds.delay + half; n++) { tms.push_back(((double)n - (double)ds.delay) * 1000.0 / wav.fs); cv.push_back(ds.c[n]); }
        jw.key("filter_ir"); jw.begin_obj(); jw.arr("t_ms", tms); jw.arr("c", cv); jw.end_obj();
    }

    // 統計
    jw.key("stats"); jw.begin_arr();
    for (int ch = 0; ch < wav.channels && ch < 16; ch++) {
        jw.begin_obj();
        jw.kv("channel", ch + 1);
        jw.kv("in_peak_db", to_db(peak_of(wav.ch[ch]))); jw.kv("out_peak_db", to_db(peak_of(outw.ch[ch])));
        jw.kv("in_rms_db", to_db(rms_of(wav.ch[ch]))); jw.kv("out_rms_db", to_db(rms_of(outw.ch[ch])));
        jw.end_obj();
    }
    jw.end_arr();
    double an_sec = t_an.sec();
    jw.key("timing"); jw.begin_obj();
    jw.kv("design_sec", design_sec); jw.kv("convolve_sec", conv_sec); jw.kv("analysis_sec", an_sec); jw.kv("total_sec", total.sec());
    jw.end_obj();
    jw.key("log"); jw.begin_arr(); for (auto& l : log) jw.val(l); jw.end_arr();
    jw.end_obj();

    if (cli.json.empty()) cli.json = dir_of(cli.out) + "result.json";
    std::string jpath = cli.json, jspath = strip_ext(cli.json) + ".js";
    if (!write_text(jpath, jw.s)) { std::fprintf(stderr, "エラー: JSON を書き込めません: %s\n", jpath.c_str()); return 6; }
    if (!write_text(jspath, "window.EQ_RESULT = " + jw.s + ";\n")) { std::fprintf(stderr, "エラー: JS を書き込めません: %s\n", jspath.c_str()); return 6; }
    std::snprintf(b, sizeof b, "[解析] %s / %s  (%.1f KB)", jpath.c_str(), jspath.c_str(), (double)jw.s.size() / 1024.0);
    std::printf("%s\n", b);
    std::snprintf(b, sizeof b, "完了: 設計 %.2fs, 畳み込み %.2fs, 解析 %.2fs, 合計 %.2fs", design_sec, conv_sec, an_sec, total.sec());
    std::printf("%s\n", b);
    return 0;
}
