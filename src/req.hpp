// req.hpp - .req 測定ファイルのローダー
//
// 対応形式 (--req-format auto で自動判別。明示指定も可):
//   java      : Java シリアライズ (java.io.ObjectOutputStream) ... 先頭 AC ED 00 05
//   text      : テキスト。1行1点、 周波数[Hz], 振幅[dB] (, 位相[deg]) 。区切りは空白/タブ/カンマ/セミコロン
//   ir-text   : テキストのインパルス応答 (1列、または 2列目を値とみなす 時間,値)
//   raw-f32 / raw-f64 : リトルエンディアン生バイナリのインパルス応答
// 「.req の中身は Java シリアライズらしい」という情報はあるが実物は未確認のため、
// Java 形式は配列名のヒューリスティックで周波数/振幅/位相を割り当て、判断が付かない場合は
// 推測せずエラーにして候補を表示する (--dump-req で構造確認、--java-map で明示指定)。
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <cstring>
#include <map>
#include <numeric>
#include <string>
#include <vector>
#include "javaser.hpp"
#include "platform.hpp"

namespace eq {

struct FreqResp {
    std::vector<double> f, mag_db, phase_deg;
    bool has_phase = false;
};

// Room EQ Wizard (REW) のフィルタファイル内の 1 フィルタ (バイクアッド)
struct EqFilter {
    std::string type;               // "PK" (ピーキング) / "NONE" (未使用スロット) など
    bool enabled = true;
    double fc = 0, gain = 0, q = 0; // 中心周波数[Hz], ゲイン[dB], Q
    double sample_rate = 48000;     // 保存時のサンプリング周波数
    bool has_stored = false;        // 保存されているバイクアッド係数があるか
    double b[3] = {1, 0, 0}, a[3] = {1, 0, 0};   // 保存係数 (a0 で正規化前の値)
};

struct ReqOptions {
    std::string format = "auto";
    double fs = 0;                 // IR のサンプリング周波数 (0 = WAV に合わせる)
    std::string mag_unit = "db";   // db | linear
    std::string phase_unit = "deg";// deg | rad
    std::string java_map;          // "freq=PATH,mag=PATH,phase=PATH,ir=PATH,fs=PATH"
};

struct ReqResult {
    FreqResp fr;
    bool is_ir = false;
    std::vector<double> ir;
    double fs_hint = 0;            // ファイル内に書かれていたサンプリング周波数 (0=不明)
    std::string detected;          // 判別結果 (ログ/JSON用)
    std::vector<std::string> log;
    std::string dump;              // --dump-req 用の構造ダンプ
    bool is_filters = false;       // REW のEQフィルタ集合 (測定データではなく、適用するEQそのもの)
    std::vector<EqFilter> filters; // 有効なフィルタのみ (NONE / 無効は除外済み)
    size_t filter_slots = 0;       // ファイル内の全スロット数
};

namespace reqdetail {

inline std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

inline bool parse_num(const std::string& t, double& v) {
    if (t.empty()) return false;
    char* end = nullptr;
    v = std::strtod(t.c_str(), &end);
    return end && *end == '\0' && std::isfinite(v);
}

inline std::vector<std::string> split_tokens(const std::string& line) {
    std::vector<std::string> out; std::string cur;
    for (char c : line) {
        if (c == ' ' || c == '\t' || c == ',' || c == ';' || c == '\r') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// UTF-16 (BOM付き) を ASCII 相当に落とす。数値データ用なので非ASCIIは '?' で十分
inline std::string decode_text(const std::vector<unsigned char>& b) {
    std::string s;
    if (b.size() >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) {
        bool le = b[0] == 0xFF;
        for (size_t i = 2; i + 1 < b.size(); i += 2) {
            unsigned lo = le ? b[i] : b[i + 1], hi = le ? b[i + 1] : b[i];
            s += (hi == 0 && lo < 0x80) ? (char)lo : '?';
        }
        return s;
    }
    size_t st = (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
    return std::string((const char*)b.data() + st, b.size() - st);
}

inline bool looks_text(const std::vector<unsigned char>& b) {
    size_t n = std::min<size_t>(b.size(), 4096), bad = 0;
    if (b.size() >= 2 && ((b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF))) return true;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = b[i];
        if (c == 0 || (c < 0x20 && c != '\t' && c != '\n' && c != '\r')) bad++;
    }
    return bad == 0;
}

// 単調増加 かつ すべて正か
inline bool monotone_positive(const std::vector<double>& v) {
    if (v.size() < 2 || v[0] <= 0) return false;
    for (size_t i = 1; i < v.size(); i++) if (!(v[i] > v[i - 1])) return false;
    return true;
}

inline std::string last_segment(std::string path) {
    // 末尾の "[...]" と ".<items>" を落として最後の '.' 以降を返す
    for (;;) {
        if (!path.empty() && path.back() == ']') { size_t i = path.rfind('['); if (i == std::string::npos) break; path.erase(i); continue; }
        const std::string tail = ".<items>";
        if (path.size() >= tail.size() && path.compare(path.size() - tail.size(), tail.size(), tail) == 0) { path.erase(path.size() - tail.size()); continue; }
        break;
    }
    size_t i = path.find_last_of('.');
    return lower(i == std::string::npos ? path : path.substr(i + 1));
}
inline std::string prefix_of(const std::string& path) {
    size_t i = path.find_last_of('.');
    return i == std::string::npos ? "" : path.substr(0, i);
}
inline bool has_any(const std::string& s, std::initializer_list<const char*> keys) {
    for (auto* k : keys) if (s.find(k) != std::string::npos) return true;
    return false;
}

enum Role { R_NONE, R_FREQ, R_MAG, R_PHASE, R_IR };
inline Role role_of(const std::string& path) {
    std::string s = last_segment(path);
    if (has_any(s, {"phase", "deg", "angle", "radian"}) || s == "ph" || s == "rad") return R_PHASE;
    if (has_any(s, {"freq", "hz", "xdata", "xval"}) || s == "f" || s == "x" || s == "xs") return R_FREQ;
    if (has_any(s, {"impulse", "coef", "taps", "kernel", "fir"}) || s == "ir" || s == "h") return R_IR;
    if (has_any(s, {"gain", "mag", "amp", "level", "db", "spl", "resp", "ydata", "yval"}) || s == "y" || s == "m") return R_MAG;
    return R_NONE;
}

inline std::string summarize(const std::vector<double>& v) {
    double mn = INFINITY, mx = -INFINITY;
    for (double x : v) { mn = std::min(mn, x); mx = std::max(mx, x); }
    char b[160];
    std::snprintf(b, sizeof b, "len=%zu min=%.6g max=%.6g%s", v.size(), mn, mx, monotone_positive(v) ? " 単調増加" : "");
    return b;
}

inline bool finalize_fr(std::vector<double> f, std::vector<double> mag, std::vector<double> ph, bool has_phase,
                        const ReqOptions& o, ReqResult& res, std::string& err) {
    size_t n = std::min(f.size(), mag.size());
    if (has_phase) n = std::min(n, ph.size());
    if (n < 2) { err = "周波数特性の点数が足りません (2点以上必要)"; return false; }
    if (o.mag_unit == "linear") for (size_t i = 0; i < n; i++) mag[i] = 20.0 * std::log10(std::max(std::fabs(mag[i]), 1e-12));
    if (has_phase && o.phase_unit == "rad") for (size_t i = 0; i < n; i++) ph[i] *= 180.0 / 3.14159265358979323846;
    std::vector<size_t> idx;
    for (size_t i = 0; i < n; i++)
        if (std::isfinite(f[i]) && std::isfinite(mag[i]) && f[i] > 0 && (!has_phase || std::isfinite(ph[i]))) idx.push_back(i);
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return f[a] < f[b]; });
    FreqResp& r = res.fr;
    r = FreqResp();
    size_t dropped_dup = 0;
    for (size_t k : idx) {
        if (!r.f.empty() && f[k] <= r.f.back()) { dropped_dup++; continue; }
        r.f.push_back(f[k]); r.mag_db.push_back(mag[k]);
        if (has_phase) r.phase_deg.push_back(ph[k]);
    }
    r.has_phase = has_phase;
    if (n - idx.size()) res.log.push_back("周波数が 0 以下/非数の点を " + std::to_string(n - idx.size()) + " 点無視しました");
    if (dropped_dup) res.log.push_back("周波数が重複する点を " + std::to_string(dropped_dup) + " 点無視しました");
    if (r.f.size() < 2) { err = "有効な周波数特性の点が 2 点未満です"; return false; }
    double mn = *std::min_element(r.mag_db.begin(), r.mag_db.end()), mx = *std::max_element(r.mag_db.begin(), r.mag_db.end());
    if (o.mag_unit == "db" && mn >= 0 && mx < 8 && !r.mag_db.empty())
        res.log.push_back("警告: 振幅値がすべて 0〜8 の範囲です。dB ではなく線形振幅の可能性があります (--req-mag-unit linear)");
    if (mx - mn > 120) res.log.push_back("警告: 振幅の変化幅が 120dB を超えています。単位 (dB/線形) を確認してください");
    return true;
}

// ---- テキスト ----
inline bool parse_text(const std::vector<unsigned char>& buf, const ReqOptions& o, bool force_ir, ReqResult& res, std::string& err) {
    std::string text = decode_text(buf);
    std::vector<std::vector<double>> rows;
    std::map<size_t, size_t> colcount;
    size_t skipped = 0;
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        auto tok = split_tokens(line);
        if (tok.empty()) continue;
        std::vector<double> v;
        for (auto& t : tok) { double x; if (parse_num(t, x)) v.push_back(x); else break; }
        bool is_data = !v.empty() && (v.size() == tok.size() || v.size() >= 2) && tok[0][0] != '#' && tok[0][0] != '*' && tok[0][0] != ';';
        if (is_data) { colcount[v.size()]++; rows.push_back(std::move(v)); }
        else {
            skipped++;
            std::string lo = lower(line);
            if (lo.find("sample") != std::string::npos || lo.find("fs") != std::string::npos) {
                for (auto& t : tok) { double x; if (parse_num(t, x) && x >= 8000 && x <= 768000) { res.fs_hint = x; break; } }
                if (res.fs_hint == 0) {  // "SampleRate=48000" のような連結形
                    size_t q = lo.find_first_of("0123456789");
                    if (q != std::string::npos) { double x = std::atof(lo.c_str() + q); if (x >= 8000 && x <= 768000) res.fs_hint = x; }
                }
            }
        }
    }
    if (rows.empty()) { err = "テキストから数値データ行を読み取れませんでした"; return false; }
    size_t mode_cols = 0, best = 0;
    for (auto& kv : colcount) if (kv.second > best) { best = kv.second; mode_cols = kv.first; }
    std::vector<std::vector<double>> use;
    for (auto& r : rows) if (r.size() == mode_cols) use.push_back(r);
    res.log.push_back("テキスト: データ行 " + std::to_string(use.size()) + " 行, " + std::to_string(mode_cols) + " 列, 非データ行 " + std::to_string(skipped) + " 行");
    if (use.size() != rows.size()) res.log.push_back("警告: 列数が異なる " + std::to_string(rows.size() - use.size()) + " 行を無視しました");

    bool as_ir = force_ir || mode_cols == 1;
    if (as_ir) {
        size_t col = mode_cols >= 2 ? 1 : 0;
        res.is_ir = true;
        res.ir.reserve(use.size());
        for (auto& r : use) res.ir.push_back(r[col]);
        res.detected = "text: インパルス応答 (" + std::to_string(res.ir.size()) + " サンプル, 列" + std::to_string(col + 1) + ")";
        return res.ir.size() >= 2 || (err = "インパルス応答が短すぎます", false);
    }
    std::vector<double> f, m, ph;
    for (auto& r : use) { f.push_back(r[0]); m.push_back(r[1]); if (mode_cols >= 3) ph.push_back(r[2]); }
    res.detected = std::string("text: 周波数, 振幅") + (mode_cols >= 3 ? ", 位相" : " (位相なし)") + " " + std::to_string(use.size()) + " 点";
    if (mode_cols > 3) res.log.push_back("注意: 4列目以降は無視しました");
    return finalize_fr(f, m, ph, mode_cols >= 3, o, res, err);
}

// ---- REW (Room EQ Wizard) フィルタファイル ----
inline void find_rew_filters(const jser::NodeP& n, std::vector<jser::NodeP>& out, std::set<const jser::Node*>& seen, int depth) {
    if (!n || depth > 60 || !seen.insert(n.get()).second) return;
    if (n->kind == jser::Node::Obj && n->cls == "roomeqwizard.Filter") { out.push_back(n); return; }
    for (auto& e : n->elems) find_rew_filters(e, out, seen, depth + 1);
    for (auto& r : n->refs) find_rew_filters(r.second, out, seen, depth + 1);
    for (auto& a : n->annot) find_rew_filters(a, out, seen, depth + 1);
}
inline bool node_prim(const jser::Node& n, const char* name, double& v) {
    for (auto& kv : n.prims) if (kv.first == name) { v = kv.second; return true; }
    return false;
}

// 戻り値: REW フィルタ形式なら true (res.is_filters を設定)。形式は合っているが内容が使えない場合は err を設定して true
inline bool try_rew_filters(const std::vector<jser::NodeP>& tops, ReqResult& res, std::string& err) {
    std::vector<jser::NodeP> fl; std::set<const jser::Node*> seen;
    for (auto& t : tops) find_rew_filters(t, fl, seen, 0);
    if (fl.empty()) return false;
    res.is_filters = true; res.filter_slots = fl.size();
    for (auto& t : tops) if (t && t->kind == jser::Node::Str) res.log.push_back("メモ: " + t->str);
    std::set<std::string> unsupported;
    for (auto& n : fl) {
        EqFilter f;
        for (auto& r : n->refs) if (r.first == "filterType" && r.second && r.second->kind == jser::Node::Enum) f.type = r.second->str;
        if (f.type.empty()) f.type = "?";
        double v = 0;
        f.enabled = !node_prim(*n, "enabled", v) || v != 0;
        node_prim(*n, "fc", f.fc); node_prim(*n, "gain", f.gain); node_prim(*n, "Q", f.q);
        if (node_prim(*n, "sampleRate", v) && v > 0) f.sample_rate = v;
        double b0, b1, b2, a0, a1, a2;
        if (node_prim(*n, "b0", b0) && node_prim(*n, "b1", b1) && node_prim(*n, "b2", b2) &&
            node_prim(*n, "a0", a0) && node_prim(*n, "a1", a1) && node_prim(*n, "a2", a2) && a0 != 0) {
            f.has_stored = true; f.b[0] = b0; f.b[1] = b1; f.b[2] = b2; f.a[0] = a0; f.a[1] = a1; f.a[2] = a2;
        }
        if (f.type == "NONE" || !f.enabled) continue;       // 未使用スロット / 無効
        if (f.type != "PK") { unsupported.insert(f.type); continue; }
        if (!(f.fc > 0) || !(f.q > 0) || !std::isfinite(f.fc) || !std::isfinite(f.q) || !std::isfinite(f.gain)) {
            err = "REW フィルタの値が不正です (fc=" + std::to_string(f.fc) + ", Q=" + std::to_string(f.q) + ")"; return true;
        }
        res.filters.push_back(f);
    }
    if (!unsupported.empty()) {
        std::string l; for (auto& u : unsupported) l += (l.empty() ? "" : ", ") + u;
        err = "未対応のREWフィルタ種別が有効になっています: " + l + "\n  確認できているのは PK (ピーキング) のみです。推測で計算すると誤った補正になるため中止します。\n  --dump-full で該当フィルタの内容を確認し、このファイルをお送りください (対応を追加します)";
        return true;
    }
    if (res.filters.empty()) { err = "REW フィルタファイルですが、有効なフィルタ (PK など) がありません (全 " + std::to_string(fl.size()) + " スロットが NONE/無効)"; return true; }
    res.detected = "java: REW (Room EQ Wizard) EQ フィルタ — 有効 " + std::to_string(res.filters.size()) + " 個 / 全 " + std::to_string(fl.size()) + " スロット";
    for (auto& f : res.filters) {
        char b[160]; std::snprintf(b, sizeof b, "%s fc=%.6g Hz  gain=%+.4g dB  Q=%.6g", f.type.c_str(), f.fc, f.gain, f.q);
        res.log.push_back(b);
    }
    return true;
}

// ---- Java シリアライズ ----
inline bool parse_java(const std::vector<unsigned char>& buf, const ReqOptions& o, ReqResult& res, std::string& err) {
    jser::Parser ps;
    if (!ps.parse(buf.data(), buf.size(), err)) return false;
    if (!ps.warning.empty()) res.log.push_back("警告: " + ps.warning);
    {
        std::set<const jser::Node*> seen;
        for (auto& t : ps.top_objects) jser::dump_node(t, 0, 0, seen, res.dump);
    }
    if (o.java_map.empty()) {                      // REW のフィルタファイルなら専用処理 (測定データではなく EQ 設定)
        std::string e2;
        if (try_rew_filters(ps.top_objects, res, e2)) { if (!e2.empty()) { err = e2; return false; } return true; }
    }
    jser::Collector col;
    col.run(ps.top_objects);
    auto& L = col.ex.lists;

    // サンプリング周波数のヒント
    for (auto& s : col.ex.scalars) {
        std::string seg = reqdetail::last_segment(s.path);
        if ((seg.find("samplerate") != std::string::npos || seg.find("samplingrate") != std::string::npos || seg.find("sample_rate") != std::string::npos || seg == "fs") &&
            s.v >= 8000 && s.v <= 768000) { res.fs_hint = s.v; break; }
    }

    auto cand_listing = [&]() {
        std::string s = "検出された数値列の候補:\n";
        for (auto& l : L) s += "    " + l.path + "  (" + summarize(l.v) + ")\n";
        s += "  → --dump-req で構造を確認し、--java-map \"freq=パス,mag=パス[,phase=パス]\" (または ir=パス) で明示指定してください";
        return s;
    };
    auto find_path = [&](const std::string& p) -> const jser::NumList* {
        for (auto& l : L) if (l.path == p) return &l;
        return nullptr;
    };

    const jser::NumList *lf = nullptr, *lm = nullptr, *lp = nullptr, *li = nullptr;
    if (!o.java_map.empty()) {
        std::map<std::string, std::string> mp;
        size_t p = 0;
        std::string last_key;
        while (p < o.java_map.size()) {
            size_t e = o.java_map.find(',', p);
            // パス中に ',' を含む "[:,0]" を許すため、'=' を含まないトークンは直前の値へ連結する
            std::string tok = o.java_map.substr(p, e == std::string::npos ? std::string::npos : e - p);
            p = (e == std::string::npos) ? o.java_map.size() : e + 1;
            size_t q = tok.find('=');
            if (q == std::string::npos) { if (!last_key.empty()) mp[last_key] += "," + tok; continue; }
            last_key = tok.substr(0, q);
            mp[last_key] = tok.substr(q + 1);
        }
        auto need_list = [&](const char* k, const jser::NumList*& dst, bool required) -> bool {
            auto it = mp.find(k);
            if (it == mp.end()) { if (required) { err = std::string("--java-map に ") + k + "= がありません\n" + cand_listing(); return false; } return true; }
            dst = find_path(it->second);
            if (!dst) { err = "--java-map で指定されたパスが見つかりません: " + it->second + "\n" + cand_listing(); return false; }
            return true;
        };
        if (mp.count("ir")) { if (!need_list("ir", li, true)) return false; }
        else { if (!need_list("freq", lf, true) || !need_list("mag", lm, true) || !need_list("phase", lp, false)) return false; }
        auto fit = mp.find("fs");
        if (fit != mp.end()) for (auto& s : col.ex.scalars) if (s.path == fit->second) res.fs_hint = s.v;
        res.log.push_back("--java-map の指定に従ってデータを割り当てました");
    } else {
        // 名前による自動割り当て。周波数列は「単調増加の正の値」であることも要求する
        std::vector<const jser::NumList*> fs_c;
        for (auto& l : L) if (l.v.size() >= 2 && role_of(l.path) == R_FREQ && monotone_positive(l.v)) fs_c.push_back(&l);
        if (!fs_c.empty()) {
            std::sort(fs_c.begin(), fs_c.end(), [](auto* a, auto* b) { return a->v.size() > b->v.size(); });
            lf = fs_c[0];
            auto pick = [&](Role r) -> const jser::NumList* {
                const jser::NumList* best = nullptr;
                for (auto& l : L) {
                    if (&l == lf || l.v.size() != lf->v.size() || role_of(l.path) != r) continue;
                    if (!best || (prefix_of(l.path) == prefix_of(lf->path) && prefix_of(best->path) != prefix_of(lf->path))) best = &l;
                }
                return best;
            };
            lm = pick(R_MAG); lp = pick(R_PHASE);
            if (!lm) { err = "周波数列 (" + lf->path + ") は見つかりましたが、同じ長さの振幅(dB)列を名前から特定できません\n" + cand_listing(); return false; }
        } else {
            std::vector<const jser::NumList*> ir_c;
            for (auto& l : L) if (l.v.size() >= 16 && role_of(l.path) == R_IR) ir_c.push_back(&l);
            if (ir_c.empty()) { err = "Java シリアライズ内に、周波数列/インパルス応答を示す名前の数値列が見つかりません\n" + cand_listing(); return false; }
            std::sort(ir_c.begin(), ir_c.end(), [](auto* a, auto* b) { return a->v.size() > b->v.size(); });
            li = ir_c[0];
        }
    }

    if (li) {
        res.is_ir = true; res.ir = li->v;
        res.detected = "java: インパルス応答 " + li->path + " (" + std::to_string(li->v.size()) + " サンプル)";
        return true;
    }
    res.detected = "java: 周波数=" + lf->path + " 振幅=" + lm->path + (lp ? " 位相=" + lp->path : std::string(" (位相なし)"));
    if (lp && lp->v.size() != lf->v.size()) { err = "位相列の長さが周波数列と一致しません"; return false; }
    if (lm->v.size() != lf->v.size()) { err = "振幅列の長さが周波数列と一致しません"; return false; }
    return finalize_fr(lf->v, lm->v, lp ? lp->v : std::vector<double>(), lp != nullptr, o, res, err);
}

}  // namespace reqdetail

// 公開API: .req を読み込む。dump_only の場合は構造ダンプのみ作る (res.dump)
inline bool load_req(const std::string& path, const ReqOptions& o, ReqResult& res, std::string& err) {
    using namespace reqdetail;
    std::vector<unsigned char> buf;
    if (!read_file(path, buf, err)) return false;
    if (buf.empty()) { err = ".req ファイルが空です"; return false; }

    std::string fmt = o.format;
    if (fmt == "auto") {
        if (buf.size() >= 4 && buf[0] == 0xAC && buf[1] == 0xED && buf[2] == 0x00 && buf[3] == 0x05) fmt = "java";
        else if (looks_text(buf)) fmt = "text";
        else { err = "形式を自動判別できません (Javaシリアライズでもテキストでもない)。--req-format raw-f32 / raw-f64 で生バイナリのインパルス応答として読めます"; return false; }
    }
    if (fmt == "java") return parse_java(buf, o, res, err);
    if (fmt == "text") return parse_text(buf, o, false, res, err);
    if (fmt == "ir-text") return parse_text(buf, o, true, res, err);
    if (fmt == "raw-f32" || fmt == "raw-f64") {
        size_t es = fmt == "raw-f32" ? 4 : 8;
        if (buf.size() % es) { err = "ファイルサイズが要素サイズの倍数ではありません"; return false; }
        size_t n = buf.size() / es;
        res.is_ir = true; res.ir.resize(n);
        for (size_t i = 0; i < n; i++) {
            if (es == 4) { float f; std::memcpy(&f, &buf[i * 4], 4); res.ir[i] = f; }
            else { double d; std::memcpy(&d, &buf[i * 8], 8); res.ir[i] = d; }
            if (!std::isfinite(res.ir[i])) res.ir[i] = 0;
        }
        res.detected = fmt + ": インパルス応答 " + std::to_string(n) + " サンプル (リトルエンディアン)";
        return true;
    }
    err = "未知の --req-format: " + fmt;
    return false;
}

}  // namespace eq
