#!/usr/bin/env python3
"""補正結果の検証: 「補正後 ≒ 元音声 × 基準レベル補正ゲイン」になっているかを数値で確認する。

  python3 verify.py DATA_DIR OUT.wav RESULT.json [--tag 名前]

  before = raw.wav  と 元音声 (ゲイン補正後) の誤差  → 補正しない場合の誤差
  after  = OUT.wav  と 元音声 (ゲイン補正後) の誤差  → 補正後の誤差
  誤差は SNR[dB] (信号電力/誤差電力) と、帯域別の残留誤差で表示する。
"""
import argparse, json, os, sys
import numpy as np
from scipy import signal
from wavio import read_wav


def snr(ref, test, trim):
    r, t = ref[trim:-trim], test[trim:-trim]
    return 10 * np.log10(np.sum(r ** 2) / max(np.sum((t - r) ** 2), 1e-30))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data"); ap.add_argument("out"); ap.add_argument("result"); ap.add_argument("--tag", default="")
    ap.add_argument("--min-gain-snr", type=float, default=None, help="この値(dB)未満なら終了コード1")
    ap.add_argument("--max-dev", type=float, default=None, help="--mag-only 時: 振幅偏差の許容最大値[dB]。超えたら終了コード1")
    ap.add_argument("--mag-only", action="store_true", help="位相を補正しない設定用: 波形一致ではなく長期スペクトルの振幅偏差で評価")
    a = ap.parse_args()
    fs, x, _ = read_wav(os.path.join(a.data, "original.wav"))
    _, raw, _ = read_wav(os.path.join(a.data, "raw.wav"))
    fs2, y, info = read_wav(a.out)
    res = json.load(open(a.result))
    assert fs == fs2 and x.shape == y.shape, "fs/shape mismatch: %s %s" % (x.shape, y.shape)
    g = 10 ** (res["meta"]["ref_level_db"] / 20)            # 正規化により出力は 元音声 × g になる
    ref = x * g
    trim = 4000
    m = res["meta"]
    # 補正は測定範囲内でのみ行う設計 (範囲外・テーパ領域は補正しない) ので、評価も補正帯域内だけで行う。
    # 帯域 = [fmin_used, fmax_used] からテーパ幅 (edge_oct) と余裕 1/3 oct を除いた範囲
    marg = m["edge_oct"] + 1 / 3
    blo = max(m["fmin_used"] * 2 ** marg, 20.0); bhi = min(m["fmax_used"] / 2 ** marg, fs / 2 * 0.95)
    bp = signal.butter(8, [blo, bhi], btype="bandpass", fs=fs, output="sos")
    print("[%s] 出力形式 tag=%d bits=%d, 基準レベル %.3f dB (g=%.4f), 評価帯域 %.0f〜%.0f Hz" % (a.tag, info[0], info[1], m["ref_level_db"], g, blo, bhi))
    worst = 1e9; maxdev = 0.0
    if a.mag_only:
        # 長期パワースペクトルの dB 差 (補正後 vs 元音声×g) を 1/6oct 平滑で比較する
        for c in range(x.shape[1]):
            f, Pr = signal.welch(ref[trim:-trim, c], fs, nperseg=16384)
            _, Py = signal.welch(y[trim:-trim, c], fs, nperseg=16384)
            _, Pw = signal.welch(raw[trim:-trim, c], fs, nperseg=16384)
            def sm(P):
                out = np.zeros_like(P)
                for i, fi in enumerate(f):
                    mm = (f >= fi / 2 ** (1 / 12)) & (f <= fi * 2 ** (1 / 12)); out[i] = P[mm].mean() if mm.any() else P[i]
                return 10 * np.log10(out + 1e-30)
            sel = (f >= blo) & (f <= bhi)
            dy = (sm(Py) - sm(Pr))[sel]; dw = (sm(Pw) - sm(Pr))[sel]
            print("  ch%d  振幅偏差(1/6oct平滑) 最大|Δ|: 補正前 %.2f dB → 補正後 %.2f dB   RMS: %.2f → %.2f dB" % (
                c + 1, np.abs(dw).max(), np.abs(dy).max(), np.sqrt((dw ** 2).mean()), np.sqrt((dy ** 2).mean())))
            maxdev = max(maxdev, np.abs(dy).max())
        if a.max_dev is not None and maxdev > a.max_dev:
            print("  NG: 振幅偏差 %.2f dB が基準 %.2f dB を超過" % (maxdev, a.max_dev)); sys.exit(1)
        print("  OK (最大偏差 %.2f dB)" % maxdev); return
    for c in range(x.shape[1]):
        rb =signal.sosfiltfilt(bp, ref[:, c]); yb = signal.sosfiltfilt(bp, y[:, c]); wb = signal.sosfiltfilt(bp, raw[:, c])
        b = snr(rb, wb, trim); af = snr(rb, yb, trim)
        worst = min(worst, af)
        print("  ch%d  帯域内SNR 補正前 %6.1f dB → 補正後 %6.1f dB  (改善 %+.1f dB)   [全帯域SNR 補正後 %.1f dB]" % (c + 1, b, af, af - b, snr(ref[:, c], y[:, c], trim)))
        # 帯域別: 誤差/信号 のパワー比
        f, Pr = signal.welch(ref[trim:-trim, c], fs, nperseg=8192)
        _, Pe = signal.welch(y[trim:-trim, c] - ref[trim:-trim, c], fs, nperseg=8192)
        _, Pb = signal.welch(raw[trim:-trim, c] - ref[trim:-trim, c], fs, nperseg=8192)
        line = "      帯域別 残留誤差(補正前→後):"
        for lo, hi in [(30, 100), (100, 500), (500, 2000), (2000, 8000), (8000, 18000)]:
            lo, hi = max(lo, blo), min(hi, bhi)
            if hi <= lo:
                continue
            m = (f >= lo) & (f < hi)
            line += "  %d-%dHz %.0f→%.0fdB" % (lo, hi, 10 * np.log10(Pb[m].sum() / Pr[m].sum()), 10 * np.log10(Pe[m].sum() / Pr[m].sum()))
        print(line)
    if a.min_gain_snr is not None and worst < a.min_gain_snr:
        print("  NG: 補正後SNR %.1f dB < 基準 %.1f dB" % (worst, a.min_gain_snr)); sys.exit(1)
    print("  OK (最悪チャンネル 補正後SNR %.1f dB)" % worst)


if __name__ == "__main__":
    main()
