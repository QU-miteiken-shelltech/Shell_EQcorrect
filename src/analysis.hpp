// analysis.hpp - 補正前後の比較用データ (Welch スペクトル, 伝達関数推定, 波形エンベロープ)
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "fft.hpp"

namespace eq {

struct SpectrumOut {
    std::vector<double> in_db, out_db;   // 平均パワースペクトル [dB] (振幅1の正弦波 = 0dB)
    std::vector<double> h_db, h_ph;      // 伝達関数 H = Gxy/Gxx の振幅[dB]と位相[deg] (入力→出力の実測)
};

// x: 入力, y: 出力 (時間が揃っていること)。grid: 対数周波数グリッド [Hz]
inline void welch_channel(const std::vector<double>& x, const std::vector<double>& y, double fs,
                          const std::vector<double>& grid, int ppo, SpectrumOut& so) {
    const size_t N = 16384, K = N / 2, hop = N / 2;
    const size_t frames = x.size();
    FFT fft(N);
    std::vector<double> win(N);
    double sw = 0;
    for (size_t n = 0; n < N; n++) { win[n] = 0.5 - 0.5 * std::cos(2 * kPi * (double)n / (double)N); sw += win[n]; }
    std::vector<double> sxx(K + 1, 0.0), syy(K + 1, 0.0), sxyr(K + 1, 0.0), sxyi(K + 1, 0.0);
    std::vector<double> re(N), im(N);
    size_t nseg = 0;
    size_t nseg_total = frames <= N ? 1 : (frames - N) / hop + 1;
    for (size_t s = 0; s < nseg_total; s++) {
        size_t base = s * hop;
        bool any = false;
        for (size_t n = 0; n < N; n++) {
            size_t idx = base + n;
            double a = idx < frames ? x[idx] : 0.0, b = idx < frames ? y[idx] : 0.0;
            if (a != 0.0 || b != 0.0) any = true;
            re[n] = a * win[n]; im[n] = b * win[n];
        }
        nseg++;
        if (!any) continue;  // 無音区間は平均に含めるが計算は省略 (ゼロ加算)
        fft.forward(re.data(), im.data());
        for (size_t k = 0; k <= K; k++) {
            size_t m = (N - k) & (N - 1);
            double a = re[k], b = im[k], c = re[m], d = im[m];
            double xr = (a + c) * 0.5, xi = (b - d) * 0.5;       // X[k]
            double yr = (b + d) * 0.5, yi = -(a - c) * 0.5;      // Y[k]
            sxx[k] += xr * xr + xi * xi;
            syy[k] += yr * yr + yi * yi;
            sxyr[k] += xr * yr + xi * yi;                         // conj(X)·Y
            sxyi[k] += xr * yi - xi * yr;
        }
    }
    const double norm = 4.0 / (sw * sw) / (double)std::max<size_t>(nseg, 1);
    const double df = fs / (double)N;
    const double half = std::pow(2.0, 1.0 / (2.0 * ppo));
    const double NaN = std::nan("");
    for (double f : grid) {
        size_t a = (size_t)std::max(1.0, std::ceil(f / half / df)), b = (size_t)std::min((double)K, std::floor(f * half / df));
        double ex = 0, ey = 0, cr = 0, ci = 0;
        if (b < a) {  // 帯域内にビンが無い (低域) → 隣接ビンの線形補間
            double xk = f / df; size_t i = std::min<size_t>((size_t)xk, K - 1); double t = xk - (double)i;
            ex = sxx[i] + t * (sxx[i + 1] - sxx[i]); ey = syy[i] + t * (syy[i + 1] - syy[i]);
            cr = sxyr[i] + t * (sxyr[i + 1] - sxyr[i]); ci = sxyi[i] + t * (sxyi[i + 1] - sxyi[i]);
        } else {
            double cnt = (double)(b - a + 1);
            for (size_t k = a; k <= b; k++) { ex += sxx[k]; ey += syy[k]; cr += sxyr[k]; ci += sxyi[k]; }
            ex /= cnt; ey /= cnt; cr /= cnt; ci /= cnt;
        }
        so.in_db.push_back(ex > 0 ? 10.0 * std::log10(ex * norm) : NaN);
        so.out_db.push_back(ey > 0 ? 10.0 * std::log10(ey * norm) : NaN);
        if (ex > 1e-24) {
            double hr = cr / ex, hi = ci / ex;
            so.h_db.push_back(20.0 * std::log10(std::max(std::hypot(hr, hi), 1e-12)));
            so.h_ph.push_back(std::atan2(hi, hr) * 180.0 / kPi);
        } else { so.h_db.push_back(NaN); so.h_ph.push_back(NaN); }
    }
}

struct Envelope { std::vector<double> t, in_min, in_max, out_min, out_max; };

inline void make_envelope(const std::vector<double>& x, const std::vector<double>& y, double fs, size_t buckets, Envelope& e) {
    size_t n = x.size();
    if (n == 0) return;
    buckets = std::min(buckets, n);
    for (size_t b = 0; b < buckets; b++) {
        size_t s = b * n / buckets, t = (b + 1) * n / buckets;
        double a0 = INFINITY, a1 = -INFINITY, b0 = INFINITY, b1 = -INFINITY;
        for (size_t i = s; i < t; i++) {
            a0 = std::min(a0, x[i]); a1 = std::max(a1, x[i]);
            b0 = std::min(b0, y[i]); b1 = std::max(b1, y[i]);
        }
        e.t.push_back(((double)s + (double)t) * 0.5 / fs);
        e.in_min.push_back(a0); e.in_max.push_back(a1); e.out_min.push_back(b0); e.out_max.push_back(b1);
    }
}

inline double rms_of(const std::vector<double>& x) {
    double s = 0; for (double v : x) s += v * v;
    return x.empty() ? 0.0 : std::sqrt(s / (double)x.size());
}
inline double peak_of(const std::vector<double>& x) {
    double p = 0; for (double v : x) p = std::max(p, std::fabs(v));
    return p;
}
inline double to_db(double lin) { return 20.0 * std::log10(std::max(lin, 1e-12)); }

}  // namespace eq
