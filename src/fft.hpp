// fft.hpp - 自前実装の複素FFT (radix-2 DIT, split-complex, double精度)
//
// ・実部/虚部を別配列で持つ (SIMD向き)
// ・ステージごとに連続したツイドルテーブルを持つ → 内側ループが連続メモリアクセス
// ・__AVX2__ 定義時 (g++/clang: -mavx2, MSVC: /arch:AVX2) は intrinsics 経路を使用
// ・逆変換は「実部と虚部を入れ替えて順変換」+ 1/N スケーリング
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <stdexcept>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace eq {

constexpr double kPi = 3.14159265358979323846264338327950288;

inline bool is_pow2(size_t n) { return n && !(n & (n - 1)); }
inline size_t next_pow2(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }

class FFT {
public:
    FFT() = default;
    explicit FFT(size_t n) { init(n); }

    void init(size_t n) {
        if (!is_pow2(n) || n < 2) throw std::runtime_error("FFT size must be power of two >= 2");
        n_ = n;
        // ビット反転テーブル
        int lg = 0; while ((size_t(1) << lg) < n) lg++;
        rev_.assign(n, 0);
        for (size_t i = 0; i < n; i++) {
            uint32_t r = 0;
            for (int b = 0; b < lg; b++) if (i & (size_t(1) << b)) r |= 1u << (lg - 1 - b);
            rev_[i] = r;
        }
        // ステージ h (=1,2,4,..,n/2) のツイドル: exp(-2πi j/(2h)), j=0..h-1 を twr_[h-1+j] に格納
        twr_.assign(n, 0.0);
        twi_.assign(n, 0.0);
        for (size_t h = 1; h < n; h <<= 1) {
            for (size_t j = 0; j < h; j++) {
                double a = -kPi * (double)j / (double)h;  // -2π j / (2h)
                twr_[h - 1 + j] = std::cos(a);
                twi_[h - 1 + j] = std::sin(a);
            }
        }
    }

    size_t size() const { return n_; }

    // 順変換 (スケーリングなし)
    void forward(double* re, double* im) const {
        const size_t n = n_;
        for (size_t i = 0; i < n; i++) {
            size_t j = rev_[i];
            if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        // h=1 (ツイドル=1)
        for (size_t i = 0; i + 1 < n; i += 2) {
            double ur = re[i], ui = im[i], vr = re[i + 1], vi = im[i + 1];
            re[i] = ur + vr; im[i] = ui + vi;
            re[i + 1] = ur - vr; im[i + 1] = ui - vi;
        }
        // h=2 (ツイドル = 1, -j)
        if (n >= 4) {
            for (size_t i = 0; i + 3 < n; i += 4) {
                double u0r = re[i], u0i = im[i], u1r = re[i + 1], u1i = im[i + 1];
                double v0r = re[i + 2], v0i = im[i + 2];
                double v1r = im[i + 3], v1i = -re[i + 3];  // (-j) * (re + j im) = im - j re
                re[i] = u0r + v0r; im[i] = u0i + v0i;
                re[i + 2] = u0r - v0r; im[i + 2] = u0i - v0i;
                re[i + 1] = u1r + v1r; im[i + 1] = u1i + v1i;
                re[i + 3] = u1r - v1r; im[i + 3] = u1i - v1i;
            }
        }
        for (size_t h = 4; h < n; h <<= 1) {
            const double* wr = &twr_[h - 1];
            const double* wi = &twi_[h - 1];
            for (size_t i = 0; i < n; i += 2 * h) {
                double* ar = re + i; double* ai = im + i;
                double* br = re + i + h; double* bi = im + i + h;
#if defined(__AVX2__)
                for (size_t j = 0; j < h; j += 4) {
                    __m256d cwr = _mm256_loadu_pd(wr + j), cwi = _mm256_loadu_pd(wi + j);
                    __m256d xr = _mm256_loadu_pd(br + j), xi = _mm256_loadu_pd(bi + j);
                    __m256d tr = _mm256_sub_pd(_mm256_mul_pd(xr, cwr), _mm256_mul_pd(xi, cwi));
                    __m256d ti = _mm256_add_pd(_mm256_mul_pd(xr, cwi), _mm256_mul_pd(xi, cwr));
                    __m256d ur = _mm256_loadu_pd(ar + j), ui = _mm256_loadu_pd(ai + j);
                    _mm256_storeu_pd(ar + j, _mm256_add_pd(ur, tr));
                    _mm256_storeu_pd(ai + j, _mm256_add_pd(ui, ti));
                    _mm256_storeu_pd(br + j, _mm256_sub_pd(ur, tr));
                    _mm256_storeu_pd(bi + j, _mm256_sub_pd(ui, ti));
                }
#else
                for (size_t j = 0; j < h; j++) {
                    double tr = br[j] * wr[j] - bi[j] * wi[j];
                    double ti = br[j] * wi[j] + bi[j] * wr[j];
                    double ur = ar[j], ui = ai[j];
                    ar[j] = ur + tr; ai[j] = ui + ti;
                    br[j] = ur - tr; bi[j] = ui - ti;
                }
#endif
            }
        }
    }

    // 逆変換 (1/N スケーリング込み)
    void inverse(double* re, double* im) const {
        forward(im, re);  // 実部と虚部を入れ替えて順変換 = 共役の逆変換
        const double s = 1.0 / (double)n_;
        for (size_t i = 0; i < n_; i++) { re[i] *= s; im[i] *= s; }
    }

private:
    size_t n_ = 0;
    std::vector<uint32_t> rev_;
    std::vector<double> twr_, twi_;
};

// 複素スペクトルの要素ごとの積: (ar + j ai) *= (br + j bi)
inline void cmul_inplace(double* ar, double* ai, const double* br, const double* bi, size_t n) {
    size_t i = 0;
#if defined(__AVX2__)
    for (; i + 4 <= n; i += 4) {
        __m256d xr = _mm256_loadu_pd(ar + i), xi = _mm256_loadu_pd(ai + i);
        __m256d yr = _mm256_loadu_pd(br + i), yi = _mm256_loadu_pd(bi + i);
        _mm256_storeu_pd(ar + i, _mm256_sub_pd(_mm256_mul_pd(xr, yr), _mm256_mul_pd(xi, yi)));
        _mm256_storeu_pd(ai + i, _mm256_add_pd(_mm256_mul_pd(xr, yi), _mm256_mul_pd(xi, yr)));
    }
#endif
    for (; i < n; i++) {
        double xr = ar[i], xi = ai[i];
        ar[i] = xr * br[i] - xi * bi[i];
        ai[i] = xr * bi[i] + xi * br[i];
    }
}

}  // namespace eq
