#!/usr/bin/env python3
"""テストデータ生成: 既知の特性 H を元音声 x に与えた「劣化音」と、その H を記述した .req 各種を作る。

  python3 make_testdata.py OUTDIR [--seconds 10] [--fs 48000]

生成物 (OUTDIR):
  original.wav        元音声 (24bit stereo) = 本来あるべき音
  raw.wav             H を通した音 (補正の入力)
  eq_text.req         周波数[Hz], 振幅[dB], 位相[deg]
  eq_text_nophase.req 周波数, 振幅のみ
  eq_ir.req           インパルス応答 (1列テキスト)
  truth.json          H の定義 (検証用)
"""
import argparse, json, os
import numpy as np
from scipy import signal
from wavio import write_wav


def pink(n, rng):
    w = rng.standard_normal(n)
    W = np.fft.rfft(w)
    f = np.arange(len(W)); f[0] = 1
    W /= np.sqrt(f)
    x = np.fft.irfft(W, n)
    return x / np.max(np.abs(x))


def peaking(f0, gain_db, q, fs):
    A = 10 ** (gain_db / 40); w0 = 2 * np.pi * f0 / fs; al = np.sin(w0) / (2 * q)
    b = [1 + al * A, -2 * np.cos(w0), 1 - al * A]; a = [1 + al / A, -2 * np.cos(w0), 1 - al / A]
    return np.array(b + a) / a[0]


def highshelf(f0, gain_db, fs, S=1.0):
    A = 10 ** (gain_db / 40); w0 = 2 * np.pi * f0 / fs; c = np.cos(w0); s = np.sin(w0)
    al = s / 2 * np.sqrt((A + 1 / A) * (1 / S - 1) + 2); sq = 2 * np.sqrt(A) * al
    b = [A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sq)]
    a = [(A + 1) - (A - 1) * c + sq, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq]
    return np.array(b + a) / a[0]


def allpass(f0, q, fs):
    w0 = 2 * np.pi * f0 / fs; al = np.sin(w0) / (2 * q)
    b = [1 - al, -2 * np.cos(w0), 1 + al]; a = [1 + al, -2 * np.cos(w0), 1 - al]
    return np.array(b + a) / a[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir"); ap.add_argument("--seconds", type=float, default=10); ap.add_argument("--fs", type=int, default=48000)
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    fs, n = a.fs, int(a.seconds * a.fs)
    rng = np.random.default_rng(1234)
    t = np.arange(n) / fs

    # 元音声: ピンクノイズ + 対数スイープ + 減衰する音階トーン
    sweep = signal.chirp(t, 30, t[-1], 18000, method="logarithmic") * 0.25
    tones = sum(0.15 * np.sin(2 * np.pi * f * t) * np.exp(-((t * 2) % 2) * 2) for f in (110, 220, 440, 880, 1760, 3520))
    L = 0.35 * pink(n, rng) + sweep + tones
    R = 0.35 * pink(n, rng) + sweep[::-1] * 0.8 + tones * 0.7
    x = np.stack([L, R], axis=1)
    x *= 0.4 / np.max(np.abs(x))

    # 既知の劣化特性 H (全体で ±8dB 程度に収まる)
    sections = [
        ("peaking", dict(f0=90, gain_db=+6.0, q=1.4)),
        ("peaking", dict(f0=320, gain_db=-5.0, q=2.0)),
        ("peaking", dict(f0=800, gain_db=-7.0, q=2.5)),
        ("peaking", dict(f0=2500, gain_db=+4.0, q=1.2)),
        ("peaking", dict(f0=6000, gain_db=-6.0, q=3.0)),
        ("highshelf", dict(f0=9000, gain_db=-4.0)),
        ("allpass", dict(f0=300, q=1.0)),
        ("allpass", dict(f0=3000, q=0.7)),
    ]
    sos = []
    for kind, p in sections:
        c = {"peaking": peaking, "highshelf": highshelf, "allpass": allpass}[kind](fs=fs, **p)
        sos.append(c)
    sos = np.array(sos)

    raw = signal.sosfilt(sos, x, axis=0)
    raw_peak = np.max(np.abs(raw))
    write_wav(os.path.join(a.outdir, "original.wav"), fs, x, 24)
    write_wav(os.path.join(a.outdir, "raw.wav"), fs, raw, 24)

    # 測定データ (対数間隔 240 点, 15Hz〜21kHz)
    f = np.geomspace(15, 21000, 240)
    _, h = signal.sosfreqz(sos, worN=f, fs=fs)
    mag = 20 * np.log10(np.abs(h)); ph = np.degrees(np.angle(h))   # 位相は ±180 に巻かれた状態
    with open(os.path.join(a.outdir, "eq_text.req"), "w") as fh:
        fh.write("# test measurement  (freq[Hz]  gain[dB]  phase[deg])\n# SampleRate: %d\n" % fs)
        for i in range(len(f)):
            fh.write("%.6f\t%.6f\t%.6f\n" % (f[i], mag[i], ph[i]))
    with open(os.path.join(a.outdir, "eq_text_nophase.req"), "w") as fh:
        fh.write("Frequency,Gain\n")
        for i in range(len(f)):
            fh.write("%.6f,%.6f\n" % (f[i], mag[i]))
    imp = np.zeros(16384); imp[0] = 1.0
    ir = signal.sosfilt(sos, imp)
    with open(os.path.join(a.outdir, "eq_ir.req"), "w") as fh:
        for v in ir:
            fh.write("%.12e\n" % v)

    json.dump({"fs": fs, "sections": sections, "raw_peak": raw_peak, "mag_range_db": [float(mag.min()), float(mag.max())]},
              open(os.path.join(a.outdir, "truth.json"), "w"), indent=1)
    print("OK: fs=%d n=%d  H範囲 %.1f〜%.1f dB  raw peak %.3f" % (fs, n, mag.min(), mag.max(), raw_peak))


if __name__ == "__main__":
    main()
