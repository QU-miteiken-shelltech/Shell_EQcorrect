// design.hpp - 補正用逆フィルタ C の設計と、FFT(オーバーラップ加算)による高精度畳み込み
//
// 設計の流れ (周波数領域で完結):
//   測定 H(f) [dB, 位相] → 基準レベル正規化 → (任意)平滑化 → 逆特性 −dB, −位相 → ブースト/カット制限
//   → 範囲端のテーパ → 遅延 N/2 の付加 (因果化) → IFFT → 窓掛け → 補正インパルス応答 c[n]
// 畳み込み:  y = A * c  を  FFT(A_block) · FFT(c) → IFFT のオーバーラップ加算で実行
//            (振幅は dB 加算、位相は測定位相の減算に相当する複素乗算)
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "fft.hpp"
#include "req.hpp"

namespace eq {

struct DesignOptions {
    size_t taps = 65536;           // FFTサイズ = フィルタ長 (2のべき乗)
    double max_boost_db = 12.0;
    double max_cut_db = 24.0;
    bool normalize = true;         // 基準帯域の平均レベルを 0dB に合わせる
    double ref_lo = 500.0, ref_hi = 2000.0;
    int smooth = 0;                // 1/N オクターブ平滑化 (0=なし)
    double edge_oct = 0.5;         // 測定範囲端で補正量を 0 に収束させる幅 [oct]
    double fmin = 0.0, fmax = 0.0; // 補正する帯域の制限 (0=測定データの範囲)
    std::string window = "tukey";  // tukey | bh | hann | none
    bool remove_delay = false;     // 測定位相の直線成分(純遅延)を補正対象から除く
    std::string phase_mode = "full";  // full(測定位相を逆に) | min(最小位相) | linear(位相補正なし)
    std::string req_role = "auto";    // EQフィルタ形式(REW)の扱い: correction(そのまま適用) | measurement(測定とみなし逆特性を作る) | auto(=correction)
};

struct DesignResult {
    std::vector<double> c;         // 補正インパルス応答 (長さ taps, 遅延 delay を含む)
    size_t delay = 0;              // 付加した遅延 (サンプル) = taps/2
    double ref_level_db = 0.0;
    std::string phase_used;        // 実際に使った位相方式
    std::vector<std::string> log;
    // 解析用 (対数グリッド上の値。測定範囲外は NaN)
    std::vector<double> grid_f;
    std::vector<double> meas_db, meas_ph, tgt_db, tgt_ph, act_db, act_ph, res_db, res_ph;
    bool meas_has_phase = false;
    double fmin_used = 0, fmax_used = 0;
    double removed_delay_ms = 0;
    int ppo = 48;                  // 解析グリッドの 1 オクターブあたりの点数
    std::string role = "measurement";  // 実際に使った扱い: measurement(逆特性を作った) | correction(EQフィルタをそのまま適用した)
};

namespace dd {

inline double smoothstep_cos(double t) { return t <= 0 ? 0.0 : t >= 1 ? 1.0 : 0.5 - 0.5 * std::cos(kPi * t); }

inline void unwrap_inplace(std::vector<double>& p) {
    for (size_t i = 1; i < p.size(); i++) {
        double d = p[i] - p[i - 1];
        p[i] = p[i - 1] + (d - 2 * kPi * std::round(d / (2 * kPi)));
    }
}

// 対数周波数軸での線形補間
struct Interp {
    std::vector<double> lf, mag, ph;
    bool has_phase = false;
    double fmin = 0, fmax = 0;
    void init(const FreqResp& r, const std::vector<double>& ph_rad) {
        lf.resize(r.f.size());
        for (size_t i = 0; i < r.f.size(); i++) lf[i] = std::log(r.f[i]);
        mag = r.mag_db; ph = ph_rad; has_phase = r.has_phase;
        fmin = r.f.front(); fmax = r.f.back();
    }
    bool eval(double f, double& m, double& p) const {
        if (f < fmin || f > fmax) return false;
        double x = std::log(f);
        size_t i = std::upper_bound(lf.begin(), lf.end(), x) - lf.begin();
        if (i == 0) i = 1;
        if (i >= lf.size()) i = lf.size() - 1;
        double t = (x - lf[i - 1]) / (lf[i] - lf[i - 1]);
        m = mag[i - 1] + t * (mag[i] - mag[i - 1]);
        p = has_phase ? ph[i - 1] + t * (ph[i] - ph[i - 1]) : 0.0;
        return true;
    }
};

// IR → 周波数特性 (Hz 単位なので IR のサンプリング周波数が WAV と違っても使える)
inline bool ir_to_fr(const std::vector<double>& ir, double fs, FreqResp& fr, std::string& err) {
    if (ir.size() < 2) { err = "インパルス応答が短すぎます"; return false; }
    size_t M = std::max<size_t>(4096, next_pow2(ir.size()) * 2);
    std::vector<double> re(M, 0.0), im(M, 0.0);
    for (size_t i = 0; i < ir.size(); i++) re[i] = ir[i];
    FFT fft(M);
    fft.forward(re.data(), im.data());
    fr = FreqResp();
    std::vector<double> ph;
    for (size_t k = 1; k <= M / 2; k++) {
        double mag = std::hypot(re[k], im[k]);
        fr.f.push_back(fs * (double)k / (double)M);
        fr.mag_db.push_back(20.0 * std::log10(std::max(mag, 1e-12)));
        ph.push_back(std::atan2(im[k], re[k]));
    }
    unwrap_inplace(ph);
    for (double p : ph) fr.phase_deg.push_back(p * 180.0 / kPi);
    fr.has_phase = true;
    return true;
}

// 振幅 (線形, 0..N/2) から最小位相の位相応答を求める (実ケプストラム法)
inline void min_phase_from_mag(const std::vector<double>& mag, size_t N, std::vector<double>& phase) {
    FFT fft(N);
    std::vector<double> re(N), im(N, 0.0);
    for (size_t k = 0; k <= N / 2; k++) {
        double l = std::log(std::max(mag[k], 1e-9));
        re[k] = l; if (k && k < N / 2) re[N - k] = l;
    }
    fft.inverse(re.data(), im.data());
    for (size_t n = 1; n < N / 2; n++) re[n] *= 2.0;
    for (size_t n = N / 2 + 1; n < N; n++) re[n] = 0.0;
    std::fill(im.begin(), im.end(), 0.0);
    fft.forward(re.data(), im.data());
    phase.assign(N / 2 + 1, 0.0);
    for (size_t k = 0; k <= N / 2; k++) phase[k] = im[k];
}

inline double window_value(const std::string& w, size_t n, size_t N) {
    const double D = (double)N / 2.0;
    if (w == "none") return 1.0;
    if (w == "hann") return 0.5 - 0.5 * std::cos(2 * kPi * (double)n / (double)N);
    if (w == "bh") {
        double x = 2 * kPi * (double)n / (double)N;
        return 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
    }
    // tukey: 中心 D から ±D の範囲のうち外側 25% (片側) を Hann テーパ
    const double alpha = 0.5;
    double d = std::fabs((double)n - D);
    double flat = D * (1.0 - alpha);
    if (d <= flat) return 1.0;
    double t = (d - flat) / (D * alpha);
    return t >= 1.0 ? 0.0 : 0.5 * (1.0 + std::cos(kPi * t));
}

}  // namespace dd

// ---- REW EQ フィルタ (バイクアッド) の周波数応答 ----
struct Bq { double b0, b1, b2, a1, a2; };

// RBJ Audio EQ Cookbook のピーキング EQ。保存係数が RBJ 式と一致することを実ファイルで確認済み
inline Bq rbj_peaking(double fc, double gain_db, double q, double fs) {
    const double A = std::pow(10.0, gain_db / 40.0), w0 = 2 * kPi * fc / fs, al = std::sin(w0) / (2 * q), c = std::cos(w0);
    const double a0 = 1 + al / A;
    return Bq{(1 + al * A) / a0, -2 * c / a0, (1 - al * A) / a0, -2 * c / a0, (1 - al / A) / a0};
}

// fs_wav での係数を作る。保存係数の fs が一致すればそれを使い (REW と同一)、違えば RBJ で再計算する。
// 保存係数と RBJ の食い違いは log に出す (式が違うフィルタを誤って計算していないかの自己点検)
inline std::vector<Bq> filter_coeffs(const std::vector<EqFilter>& fl, double fs_wav, std::vector<std::string>& log) {
    std::vector<Bq> out; double worst = 0; bool used_recalc = false;
    for (auto& f : fl) {
        Bq r = rbj_peaking(f.fc, f.gain, f.q, f.sample_rate);
        if (f.has_stored) {
            Bq s{f.b[0] / f.a[0], f.b[1] / f.a[0], f.b[2] / f.a[0], f.a[1] / f.a[0], f.a[2] / f.a[0]};
            const double d[5] = {s.b0 - r.b0, s.b1 - r.b1, s.b2 - r.b2, s.a1 - r.a1, s.a2 - r.a2};
            for (double x : d) worst = std::max(worst, std::fabs(x));
            if (std::fabs(f.sample_rate - fs_wav) < 0.5) { out.push_back(s); continue; }
        }
        used_recalc = true;
        out.push_back(rbj_peaking(f.fc, f.gain, f.q, fs_wav));
    }
    if (worst > 1e-6) log.push_back("警告: 保存されている係数が RBJ ピーキング式と一致しません (最大差 " + std::to_string(worst) + ")。ファイルの計算式が想定と違う可能性があります");
    if (used_recalc) log.push_back("注意: .req の保存サンプリング周波数と WAV (" + std::to_string((int)fs_wav) + " Hz) が異なるため、fc/ゲイン/Q から RBJ 式で係数を再計算しました (REW が同じ式で再計算するかは未確認)");
    return out;
}

// 周波数 f[Hz] での縦続応答。位相は各段の和 (巻き戻しなし)
inline void cascade_response(const std::vector<Bq>& c, double fs, double f, double& db, double& ph) {
    const double w = 2 * kPi * f / fs, c1 = std::cos(w), s1 = -std::sin(w), c2 = std::cos(2 * w), s2 = -std::sin(2 * w);
    double lg = 0, p = 0;
    for (auto& q : c) {
        const double nr = q.b0 + q.b1 * c1 + q.b2 * c2, ni = q.b1 * s1 + q.b2 * s2;
        const double dr = 1 + q.a1 * c1 + q.a2 * c2, di = q.a1 * s1 + q.a2 * s2;
        lg += 0.5 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
        p += std::atan2(ni, nr) - std::atan2(di, dr);
    }
    db = 20.0 * lg; ph = p;
}

// 補正スペクトル (振幅[dB]・位相[rad], 0..N/2 ビン) → 補正インパルス応答 c[n] と、解析用データ
//   遅延 N/2 を付加して因果化 → IFFT → 窓 → (窓掛け後の) 実際の応答を再計算 → 対数グリッドへ
inline bool finish_design(const std::vector<double>& corr_db, const std::vector<double>& corr_ph, const dd::Interp& ip,
                          bool meas_has_phase, double L0, double lo, double hi, const std::string& mode,
                          double fs_wav, const DesignOptions& o, DesignResult& out) {
    using namespace dd;
    const size_t N = o.taps, K = N / 2;
    const double df = fs_wav / (double)N, nyq = fs_wav / 2.0;
    const bool fr_has_phase = meas_has_phase;
    // --- スペクトル → IFFT ---
    std::vector<double> re(N, 0.0), im(N, 0.0);
    for (size_t k = 0; k <= K; k++) {
        double m = std::pow(10.0, corr_db[k] / 20.0);
        double ph_k = corr_ph[k] - kPi * (double)k;   // 遅延 N/2 サンプル = exp(-jπk)
        re[k] = m * std::cos(ph_k); im[k] = m * std::sin(ph_k);
    }
    im[0] = 0.0; im[K] = 0.0;                          // DC / Nyquist は実数
    for (size_t k = 1; k < K; k++) { re[N - k] = re[k]; im[N - k] = -im[k]; }
    FFT fft(N);
    fft.inverse(re.data(), im.data());
    out.c.resize(N);
    for (size_t n = 0; n < N; n++) out.c[n] = re[n] * window_value(o.window, n, N);
    out.delay = N / 2;

    // --- 実際のフィルタ応答 (窓掛け後) ---
    std::fill(im.begin(), im.end(), 0.0);
    for (size_t n = 0; n < N; n++) re[n] = out.c[n];
    fft.forward(re.data(), im.data());
    std::vector<double> a_db(K + 1), a_ph(K + 1);
    for (size_t k = 0; k <= K; k++) {
        double r = (k & 1) ? -re[k] : re[k], i = (k & 1) ? -im[k] : im[k];   // 遅延除去 (-1)^k
        a_db[k] = 20.0 * std::log10(std::max(std::hypot(r, i), 1e-12));
        a_ph[k] = std::atan2(i, r);
    }
    unwrap_inplace(a_ph);

    // --- 解析用: 対数グリッドへ ---
    const int ppo = out.ppo;
    for (double f = 20.0; f <= nyq * 0.9999; f *= std::pow(2.0, 1.0 / ppo)) out.grid_f.push_back(f);
    const double NaN = std::nan("");
    auto lin_at = [&](const std::vector<double>& v, double f) {
        double x = f / df; size_t i = (size_t)x; if (i >= K) return v[K];
        double t = x - (double)i; return v[i] + t * (v[i + 1] - v[i]);
    };
    for (double f : out.grid_f) {
        double m, p;
        bool in = ip.eval(f, m, p);
        double mn = in ? m - L0 : NaN;
        double pn = (in && fr_has_phase) ? p * 180.0 / kPi : NaN;
        out.meas_db.push_back(mn); out.meas_ph.push_back(pn);
        double td = (f >= lo && f <= hi) ? lin_at(corr_db, f) : (in ? 0.0 : NaN);
        double tp = lin_at(corr_ph, f) * 180.0 / kPi;
        out.tgt_db.push_back(td); out.tgt_ph.push_back(mode == "linear" ? NaN : tp);
        double ad = lin_at(a_db, f), ap = lin_at(a_ph, f) * 180.0 / kPi;
        out.act_db.push_back(ad); out.act_ph.push_back(ap);
        out.res_db.push_back(in ? mn + ad : NaN);
        out.res_ph.push_back((in && fr_has_phase) ? pn + ap : NaN);
    }
    return true;
}

// REW の EQ フィルタ集合をそのまま補正として適用する (測定の逆ではない)。
// バイクアッドは最小位相なので、位相補正にプリリンギングは伴わない。基準レベル正規化・ブースト/カット上限・端のテーパは適用しない
// (EQ の設定値そのものを忠実に再現するため)。
inline bool design_from_filters(const ReqResult& req, double fs_wav, const DesignOptions& o, DesignResult& out, std::string& err) {
    using namespace dd;
    if (!is_pow2(o.taps) || o.taps < 1024) { err = "--taps は 1024 以上の 2 のべき乗にしてください"; return false; }
    const size_t N = o.taps, K = N / 2;
    const double df = fs_wav / (double)N, nyq = fs_wav / 2.0;
    std::vector<Bq> cf = filter_coeffs(req.filters, fs_wav, out.log);
    for (auto& f : req.filters)
        if (f.fc >= nyq) { err = "フィルタの中心周波数 " + std::to_string(f.fc) + " Hz が WAV のナイキスト周波数 " + std::to_string(nyq) + " Hz 以上です"; return false; }
    std::vector<double> corr_db(K + 1), corr_ph(K + 1);
    for (size_t k = 0; k <= K; k++) cascade_response(cf, fs_wav, (double)k * df, corr_db[k], corr_ph[k]);
    unwrap_inplace(corr_ph);
    Interp none; none.fmin = none.fmax = 1e30;            // 測定データなし (eval は常に false → 測定/残差は NaN)
    out.meas_has_phase = false; out.ref_level_db = 0.0; out.phase_used = "filters"; out.role = "correction";
    out.fmin_used = df; out.fmax_used = nyq;
    out.log.push_back("EQ フィルタ " + std::to_string(req.filters.size()) + " 個をそのまま補正として適用 (基準レベル正規化・補正量上限・端のテーパは適用しません)");
    return finish_design(corr_db, corr_ph, none, false, 0.0, df, nyq, "full", fs_wav, o, out);
}

inline bool design_filter(const ReqResult& req, double fs_wav, const DesignOptions& o, DesignResult& out, std::string& err) {
    using namespace dd;
    if (!is_pow2(o.taps) || o.taps < 1024) { err = "--taps は 1024 以上の 2 のべき乗にしてください"; return false; }
    const size_t N = o.taps;
    const double df = fs_wav / (double)N;
    const double nyq = fs_wav / 2.0;

    // --- REW の EQ フィルタ: 既定ではそのフィルタ自体を補正として適用する ---
    if (req.is_filters) {
        std::string role = o.req_role == "auto" ? "correction" : o.req_role;
        if (role != "correction" && role != "measurement") { err = "--req-role は correction か measurement"; return false; }
        out.role = role;
        if (role == "correction") return design_from_filters(req, fs_wav, o, out, err);
    } else if (o.req_role == "correction") {
        err = "--req-role correction は REW の EQ フィルタ形式の .req でのみ使えます (この .req は測定データです)"; return false;
    }

    // --- 測定データの準備 ---
    FreqResp fr = req.fr;
    if (req.is_filters) {          // role=measurement: EQ フィルタの合成応答を「測定された特性」とみなし、その逆特性を作る
        std::vector<Bq> cf = filter_coeffs(req.filters, fs_wav, out.log);
        fr = FreqResp(); std::vector<double> ph;
        for (double f = 10.0; f < fs_wav / 2 * 0.99; f *= std::pow(2.0, 1.0 / 48)) {
            double db, p; cascade_response(cf, fs_wav, f, db, p);
            fr.f.push_back(f); fr.mag_db.push_back(db); ph.push_back(p);
        }
        unwrap_inplace(ph);
        for (double p : ph) fr.phase_deg.push_back(p * 180.0 / kPi);
        fr.has_phase = true;
        out.log.push_back("EQ フィルタの合成応答を測定特性とみなして逆特性を作成します (--req-role measurement)");
    } else if (req.is_ir) {
        double fs_ir = (req.fs_hint > 0 ? req.fs_hint : fs_wav);
        std::string e2;
        if (!ir_to_fr(req.ir, fs_ir, fr, e2)) { err = e2; return false; }
        out.log.push_back("インパルス応答 → 周波数特性に変換 (IRのfs=" + std::to_string((int)fs_ir) + "Hz, " + std::to_string(req.ir.size()) + "サンプル)");
    }
    std::vector<double> ph_rad;
    if (fr.has_phase) {
        ph_rad.resize(fr.phase_deg.size());
        for (size_t i = 0; i < ph_rad.size(); i++) ph_rad[i] = fr.phase_deg[i] * kPi / 180.0;
        unwrap_inplace(ph_rad);
    }
    out.meas_has_phase = fr.has_phase;
    if (fr.has_phase && o.remove_delay) {
        // φ = a + b f の最小二乗 → b·f を除去 (b = -2π τ)
        double sx = 0, sy = 0, sxx = 0, sxy = 0; size_t n = fr.f.size();
        for (size_t i = 0; i < n; i++) { sx += fr.f[i]; sy += ph_rad[i]; sxx += fr.f[i] * fr.f[i]; sxy += fr.f[i] * ph_rad[i]; }
        double den = (double)n * sxx - sx * sx;
        if (std::fabs(den) > 0) {
            double b = ((double)n * sxy - sx * sy) / den;
            for (size_t i = 0; i < n; i++) ph_rad[i] -= b * fr.f[i];
            out.removed_delay_ms = -b / (2 * kPi) * 1000.0;
            char buf[128]; std::snprintf(buf, sizeof buf, "測定位相の直線成分(遅延 %.4f ms)を除去しました", out.removed_delay_ms);
            out.log.push_back(buf);
        }
    }
    Interp ip; ip.init(fr, ph_rad);

    double lo = std::max(ip.fmin, o.fmin > 0 ? o.fmin : 0.0);
    double hi = std::min(ip.fmax, o.fmax > 0 ? o.fmax : 1e12);
    hi = std::min(hi, nyq);
    if (!(hi > lo)) { err = "補正可能な周波数範囲がありません (測定範囲 " + std::to_string(ip.fmin) + "〜" + std::to_string(ip.fmax) + "Hz, WAVのナイキスト " + std::to_string(nyq) + "Hz)"; return false; }
    out.fmin_used = lo; out.fmax_used = hi;
    if (ip.fmax < nyq * 0.5) out.log.push_back("注意: 測定範囲の上限 (" + std::to_string((int)ip.fmax) + "Hz) がナイキストの半分未満です。それより上は補正しません");

    // --- 設計グリッド (0..N/2 bin) に補間 ---
    const size_t K = N / 2;
    std::vector<double> mag(K + 1, 0.0), ph(K + 1, 0.0);
    std::vector<char> valid(K + 1, 0);
    for (size_t k = 1; k <= K; k++) {
        double f = (double)k * df, m, p;
        if (ip.eval(f, m, p)) { mag[k] = m; ph[k] = p; valid[k] = 1; }
    }
    // 基準レベル
    double L0 = 0.0;
    if (o.normalize) {
        double sw = 0, sm = 0;
        for (size_t k = 1; k <= K; k++) {
            double f = (double)k * df;
            if (valid[k] && f >= o.ref_lo && f <= o.ref_hi) { sw += 1.0 / f; sm += mag[k] / f; }
        }
        if (sw == 0) {
            for (size_t k = 1; k <= K; k++) if (valid[k]) { double f = (double)k * df; sw += 1.0 / f; sm += mag[k] / f; }
            out.log.push_back("警告: 基準帯域 " + std::to_string((int)o.ref_lo) + "〜" + std::to_string((int)o.ref_hi) + "Hz に測定データがないため、測定範囲全体の平均を基準にしました");
        }
        L0 = sm / sw;
    }
    out.ref_level_db = L0;

    // 測定値 (正規化済み・平滑化前) を保持
    std::vector<double> mag_raw = mag, ph_raw = ph;

    // --- 平滑化 (1/N oct, 有効ビンのみ) ---
    if (o.smooth > 0) {
        std::vector<double> sm(K + 2, 0.0), sp(K + 2, 0.0), cnt(K + 2, 0.0);
        for (size_t k = 0; k <= K; k++) {
            sm[k + 1] = sm[k] + (valid[k] ? mag[k] : 0.0);
            sp[k + 1] = sp[k] + (valid[k] ? ph[k] : 0.0);
            cnt[k + 1] = cnt[k] + (valid[k] ? 1.0 : 0.0);
        }
        const double r = std::pow(2.0, 1.0 / (2.0 * o.smooth));
        std::vector<double> m2 = mag, p2 = ph;
        for (size_t k = 1; k <= K; k++) {
            if (!valid[k]) continue;
            size_t a = (size_t)std::max(1.0, std::ceil((double)k / r)), b = (size_t)std::min((double)K, std::floor((double)k * r));
            if (b < a) { a = b = k; }
            double c = cnt[b + 1] - cnt[a];
            if (c > 0) { m2[k] = (sm[b + 1] - sm[a]) / c; p2[k] = (sp[b + 1] - sp[a]) / c; }
        }
        mag.swap(m2); ph.swap(p2);
        out.log.push_back("測定データを 1/" + std::to_string(o.smooth) + " オクターブで平滑化してから逆特性を作成");
    }

    // --- 逆特性 ---
    std::vector<double> corr_db(K + 1, 0.0), w(K + 1, 0.0);
    size_t n_boost_lim = 0, n_cut_lim = 0;
    for (size_t k = 1; k <= K; k++) {
        double f = (double)k * df;
        if (!valid[k] || f < lo || f > hi) continue;
        double wt = 1.0;
        if (o.edge_oct > 0) wt = smoothstep_cos(std::log2(f / lo) / o.edge_oct) * smoothstep_cos(std::log2(hi / f) / o.edge_oct);
        double c = -(mag[k] - L0);
        if (c > o.max_boost_db) { c = o.max_boost_db; n_boost_lim++; }
        if (c < -o.max_cut_db) { c = -o.max_cut_db; n_cut_lim++; }
        corr_db[k] = c * wt;
        w[k] = wt;
    }
    if (n_boost_lim) out.log.push_back("ブースト制限 (+" + std::to_string((int)o.max_boost_db) + "dB) が " + std::to_string(n_boost_lim) + " ビンで働きました");
    if (n_cut_lim) out.log.push_back("カット制限 (-" + std::to_string((int)o.max_cut_db) + "dB) が " + std::to_string(n_cut_lim) + " ビンで働きました");

    // --- 位相 ---
    std::string mode = o.phase_mode;
    if (mode == "full" && !fr.has_phase) {
        mode = "min";
        out.log.push_back("測定データに位相がないため、位相は振幅から最小位相として推定しました");
    }
    std::vector<double> corr_ph(K + 1, 0.0);
    if (mode == "full") {
        // 位相は測定位相の逆をそのまま使う (振幅のようなテーパはかけない: 端の位相は数百度に達しうるため、
        // テーパすると位相が合わなくなり群遅延も暴れる)。補正帯域の外側は連続につなぐ:
        //   上側: 最後の値を保持 (追加の群遅延なし) / 下側: DC の 0 へ周波数に比例して接続
        size_t klo = 0, khi = 0;
        for (size_t k = 1; k <= K; k++) { double f = (double)k * df; if (valid[k] && f >= lo && f <= hi) { if (!klo) klo = k; khi = k; } }
        if (klo) {
            for (size_t k = 1; k <= K; k++) {
                if (k < klo) corr_ph[k] = -ph[klo] * ((double)k / (double)klo);
                else if (k <= khi) corr_ph[k] = -ph[k];
                else corr_ph[k] = -ph[khi];
            }
        }
    } else if (mode == "min") {
        std::vector<double> lin(K + 1);
        for (size_t k = 0; k <= K; k++) lin[k] = std::pow(10.0, corr_db[k] / 20.0);
        min_phase_from_mag(lin, N, corr_ph);
    } else if (mode == "linear") {
        // 位相補正なし (線形位相: 遅延のみ)
    } else { err = "未知の --phase: " + o.phase_mode; return false; }
    out.phase_used = mode;

    (void)mag_raw; (void)ph_raw;
    return finish_design(corr_db, corr_ph, ip, fr.has_phase, L0, lo, hi, mode, fs_wav, o, out);
}

// ---------------------------------------------------------------------------
// オーバーラップ加算による FFT 畳み込み
//   y[t] = (x * c)[t + delay]  (設計時に付加した遅延 delay を打ち消し、入力と時間が揃う)
//   2 チャンネルを実部/虚部に詰めて 1 回の FFT で処理する (c が実数なので線形性により分離できる)
// ---------------------------------------------------------------------------
class Convolver {
public:
    Convolver(const std::vector<double>& c, size_t delay, size_t frames) : c_(c), delay_(delay), frames_(frames) {
        const size_t N = c.size();
        const size_t total = frames + delay;                 // 出力に必要な長さ (遅延分の先送り)
        size_t target_block = std::min<size_t>(N, std::max<size_t>(total, 1));
        M_ = next_pow2(N + target_block - 1);
        if (M_ < 2048) M_ = 2048;
        B_ = M_ - N + 1;                                      // 1ブロックの入力サンプル数
        fft_.init(M_);
        cr_.assign(M_, 0.0); ci_.assign(M_, 0.0);
        for (size_t i = 0; i < N; i++) cr_[i] = c[i];
        fft_.forward(cr_.data(), ci_.data());                // C = FFT(c) を一度だけ計算
        xr_.assign(M_, 0.0); xi_.assign(M_, 0.0);
    }
    size_t fft_size() const { return M_; }
    size_t block() const { return B_; }

    // x2/y2 は nullptr 可 (モノラル/奇数チャンネル)
    void process(const std::vector<double>& x1, const std::vector<double>* x2, std::vector<double>& y1, std::vector<double>* y2) {
        y1.assign(frames_, 0.0);
        if (y2) y2->assign(frames_, 0.0);
        const size_t total = frames_ + delay_;
        for (size_t pos = 0; pos < total; pos += B_) {
            size_t len = std::min(B_, total - pos);
            std::fill(xr_.begin(), xr_.end(), 0.0);
            std::fill(xi_.begin(), xi_.end(), 0.0);
            for (size_t i = 0; i < len && pos + i < frames_; i++) {
                xr_[i] = x1[pos + i];
                if (x2) xi_[i] = (*x2)[pos + i];
            }
            fft_.forward(xr_.data(), xi_.data());            // A = FFT(x block)
            cmul_inplace(xr_.data(), xi_.data(), cr_.data(), ci_.data(), M_);   // A · C
            fft_.inverse(xr_.data(), xi_.data());            // IFFT
            // オーバーラップ加算: 出力時刻 t = pos + i - delay
            size_t outlen = len + c_.size() - 1;
            for (size_t i = 0; i < outlen; i++) {
                size_t tt = pos + i;
                if (tt < delay_) continue;
                tt -= delay_;
                if (tt >= frames_) break;
                y1[tt] += xr_[i];
                if (y2) (*y2)[tt] += xi_[i];
            }
        }
    }

private:
    const std::vector<double>& c_;
    size_t delay_, frames_, M_ = 0, B_ = 0;
    FFT fft_;
    std::vector<double> cr_, ci_, xr_, xi_;
};

}  // namespace eq
