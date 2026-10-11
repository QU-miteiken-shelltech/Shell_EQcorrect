#!/usr/bin/env python3
"""入出力フォーマット・チャンネル構成・端のケースの回帰テスト (終了コード 0 = 全合格)
  python3 tests/test_formats.py [eqcorrect実行ファイル]
前提: tests/data/ に make_testdata.py の出力があること"""
import os, struct, subprocess, sys, tempfile
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from wavio import read_wav

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "eqcorrect")
DATA = os.path.join(ROOT, "tests", "data")
REQ = os.path.join(DATA, "eq_text.req")
tmp = tempfile.mkdtemp()
fails = []


def check(name, cond, detail=""):
    print(("  PASS " if cond else "  FAIL ") + name + (("  - " + detail) if detail else ""))
    if not cond:
        fails.append(name)


def write_raw(path, fs, x, kind, ext=False):
    """kind: u8 / i16 / i24 / i32 / f32 / f64。x: (frames, ch)"""
    n, ch = x.shape
    if kind == "u8":
        d = (np.clip(np.round(x * 128 + 128), 0, 255)).astype(np.uint8).tobytes(); bits, tag = 8, 1
    elif kind == "i16":
        d = np.clip(np.round(x * 32768), -32768, 32767).astype("<i2").tobytes(); bits, tag = 16, 1
    elif kind == "i24":
        iv = np.clip(np.round(x * 8388608), -8388608, 8388607).astype(np.int64) & 0xFFFFFF
        d = np.stack([iv & 255, (iv >> 8) & 255, (iv >> 16) & 255], -1).astype(np.uint8).tobytes(); bits, tag = 24, 1
    elif kind == "i32":
        d = np.clip(np.round(x * 2147483648), -2147483648, 2147483647).astype("<i4").tobytes(); bits, tag = 32, 1
    elif kind == "f32":
        d = x.astype("<f4").tobytes(); bits, tag = 32, 3
    else:
        d = x.astype("<f8").tobytes(); bits, tag = 64, 3
    if ext:
        fmt = struct.pack("<HHIIHH", 0xFFFE, ch, fs, fs * ch * bits // 8, ch * bits // 8, bits) + struct.pack("<HHI", 22, bits, 0) + struct.pack("<H", tag) + bytes.fromhex("000000001000800000AA00389B71")
    else:
        fmt = struct.pack("<HHIIHH", tag, ch, fs, fs * ch * bits // 8, ch * bits // 8, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(d)) + d
    if len(d) & 1:
        body += b"\0"
    open(path, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)


def run(inp, out, *extra, req=REQ):
    js = out + ".json"
    p = subprocess.run([EXE, inp, req, "-o", out, "-j", js, *extra], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


fs, x0, _ = read_wav(os.path.join(DATA, "raw.wav"))
x0 = x0[: fs * 3]            # 3 秒に短縮

# ---- 基準: ステレオ float32 → float32 出力 (他と比較する基準結果)
ref_in = os.path.join(tmp, "ref.wav"); write_raw(ref_in, fs, x0, "f32")
ref_out = os.path.join(tmp, "ref_out.wav")
rc, log = run(ref_in, ref_out, "--bits", "f32")
check("基準処理 (stereo float32)", rc == 0, log[-200:] if rc else "")
_, yref, info = read_wav(ref_out)
check("float32 出力で tag=3, 32bit", info == (3, 32), str(info))

print("== 入力ビット深度 (出力は同一ビット深度の既定動作)")
for kind, bits, tag in [("u8", 16, 1), ("i16", 16, 1), ("i24", 24, 1), ("i32", 32, 1), ("f32", 32, 3), ("f64", 32, 3)]:
    p = os.path.join(tmp, "in_%s.wav" % kind); write_raw(p, fs, x0, kind)
    o = os.path.join(tmp, "out_%s.wav" % kind)
    rc, log = run(p, o)
    ok = rc == 0
    if ok:
        _, y, inf = read_wav(o)
        check("%s 入力 → 出力形式 tag=%d bits=%d" % (kind, tag, bits), inf == (tag, bits) and y.shape == x0.shape, "got %s %s" % (inf, y.shape))
        tol = {"u8": 0.05, "i16": 5e-4, "i24": 5e-6, "i32": 1e-6, "f32": 1e-6, "f64": 1e-6}[kind]
        err = np.max(np.abs(y - yref))
        check("%s 入力の結果が float32 基準と一致 (最大差 %.2e < %.0e)" % (kind, err, tol), err < tol)
    else:
        check("%s 入力の処理" % kind, False, log[-200:])

print("== チャンネル構成 (各チャンネルは独立に同じ結果になること)")
for nch in (1, 3, 6):
    cols = [x0[:, i % 2] for i in range(nch)]
    X = np.stack(cols, 1)
    p = os.path.join(tmp, "ch%d.wav" % nch); write_raw(p, fs, X, "f32", ext=(nch > 2))
    o = os.path.join(tmp, "ch%d_out.wav" % nch)
    rc, log = run(p, o, "--bits", "f32")
    if rc != 0:
        check("%dch 処理" % nch, False, log[-200:]); continue
    _, y, _ = read_wav(o)
    errs = [np.max(np.abs(y[:, i] - yref[:, i % 2])) for i in range(nch)]
    check("%dch: 全チャンネルがステレオ基準と一致 (最大差 %.1e)" % (nch, max(errs)), y.shape == X.shape and max(errs) < 1e-9)

print("== 畳み込みの独立検証 (出力 vs numpy fftconvolve。短いファイル/ブロック境界/各フィルタ長)")
from scipy.signal import fftconvolve
def conv_check(label, X, *extra):
    p = os.path.join(tmp, "cc_in.wav"); write_raw(p, fs, X, "f32")
    o = os.path.join(tmp, "cc_out.wav"); cf = os.path.join(tmp, "cc_filt.wav")
    rc, log = run(p, o, "--bits", "f32", "--save-filter", cf, *extra)
    if rc != 0:
        check(label, False, log[-200:]); return
    _, y, _ = read_wav(o); _, c, _ = read_wav(cf)
    c = c[:, 0]; D = len(c) // 2
    worst = 0.0
    for ch in range(X.shape[1]):
        # float32 に丸められた入力 (WAV に書いた値) に対する厳密な畳み込み
        xin = X[:, ch].astype(np.float32).astype(np.float64)
        exp = fftconvolve(xin, c)[D:D + len(xin)]
        worst = max(worst, np.max(np.abs(y[:, ch] - exp)))
    check("%s: 最大差 %.1e (< 3e-6)" % (label, worst), y.shape == X.shape and worst < 3e-6)

for n in (1, 2, 100, 5000):
    conv_check("%d サンプル (taps 65536)" % n, x0[:n])
conv_check("3秒 / taps 65536 (ブロック数が少ない)", x0)
conv_check("3秒 / taps 4096 (多数のブロック境界)", x0, "--taps", "4096")
conv_check("3秒 / taps 1024 (最小フィルタ長)", x0, "--taps", "1024")
conv_check("3秒 / taps 262144 (ファイルより長いフィルタ)", x0, "--taps", "262144")
X3 = np.stack([x0[:, 0], x0[:, 1], x0[:, 0] * 0.5], 1)
conv_check("3ch (奇数チャンネルの単独処理) / taps 4096", X3, "--taps", "4096")
conv_check("--phase min / taps 4096", x0, "--taps", "4096", "--phase", "min")
conv_check("--phase linear / taps 4096", x0, "--taps", "4096", "--phase", "linear")

print("== 因果性 (全位相補正は未来サンプルに依存する設計であることの確認)")
write_raw(os.path.join(tmp, "imp.wav"), fs, np.concatenate([np.zeros((20000, 1)), np.ones((1, 1)), np.zeros((20000, 1))]), "f32")
rc, log = run(os.path.join(tmp, "imp.wav"), os.path.join(tmp, "imp_out.wav"), "--bits", "f32", "--taps", "4096")
_, yi, _ = read_wav(os.path.join(tmp, "imp_out.wav"))
pre = np.max(np.abs(yi[:20000 - 100, 0]))
check("インパルス入力: 位置より前にプリリンギングが出る (最大 %.1e > 1e-6)" % pre, pre > 1e-6)
rc, log = run(os.path.join(tmp, "imp.wav"), os.path.join(tmp, "imp_out2.wav"), "--bits", "f32", "--taps", "4096", "--phase", "min")
_, yi2, _ = read_wav(os.path.join(tmp, "imp_out2.wav"))
pre2 = np.max(np.abs(yi2[:20000 - 100, 0]))
check("--phase min では最小位相ゆえプリリンギングが大幅に小さい (%.1e < full の %.1e)" % (pre2, pre), pre2 < pre * 0.1)

# 0 フレーム / 壊れた WAV / 存在しないファイル / 非対応形式
p = os.path.join(tmp, "empty.wav"); write_raw(p, fs, x0[:0], "f32")
rc, log = run(p, os.path.join(tmp, "e.wav")); check("0フレームWAVはエラー終了 (rc=%d)" % rc, rc != 0)
open(os.path.join(tmp, "junk.wav"), "wb").write(b"RIFFxxxxWAVEjunk")
rc, log = run(os.path.join(tmp, "junk.wav"), os.path.join(tmp, "e.wav")); check("壊れたWAVはエラー終了 (rc=%d)" % rc, rc != 0)
rc, log = run(os.path.join(tmp, "nonexist.wav"), os.path.join(tmp, "e.wav")); check("存在しないWAVはエラー終了 (rc=%d)" % rc, rc != 0)
trunc = open(ref_in, "rb").read()[:5000]; open(os.path.join(tmp, "trunc.wav"), "wb").write(trunc)
rc, log = run(os.path.join(tmp, "trunc.wav"), os.path.join(tmp, "e.wav"), "--bits", "f32")
check("データ途中で切れたWAVは読めた分だけ処理 (rc=%d)" % rc, rc == 0)
open(os.path.join(tmp, "bad.req"), "w").write("this is not a measurement\n")
rc, log = run(ref_in, os.path.join(tmp, "e.wav"), req=os.path.join(tmp, "bad.req")); check("数値のない .req はエラー終了 (rc=%d)" % rc, rc != 0)
open(os.path.join(tmp, "one.req"), "w").write("100 0 0\n")
rc, log = run(ref_in, os.path.join(tmp, "e.wav"), req=os.path.join(tmp, "one.req")); check("1点だけの .req はエラー終了 (rc=%d)" % rc, rc != 0)

print("== REW (Room EQ Wizard) の EQ フィルタ形式 (.req)  ※基準は scipy の IIR (sosfilt) による独立計算")
from scipy import signal as _sg
def rbj(fc, g, q, fsr):
    A = 10 ** (g / 40); w0 = 2 * np.pi * fc / fsr; al = np.sin(w0) / (2 * q); c = np.cos(w0); a0 = 1 + al / A
    return [(1 + al * A) / a0, -2 * c / a0, (1 - al * A) / a0, 1, -2 * c / a0, (1 - al / A) / a0]
def rew_run(name, X, fsr, *extra):
    p = os.path.join(tmp, "rew_in.wav"); write_raw(p, fsr, X, "f32")
    o = os.path.join(tmp, "rew_out.wav")
    rc, log = run(p, o, "--bits", "f32", *extra, req=os.path.join(DATA, name + ".req"))
    return rc, log, (read_wav(o)[1] if rc == 0 else None)
FIVE = [(57.5, -10.1, 2.539), (86.5, -8.2, 4.299), (142, -11.3, 1.599), (277, -6.1, 1.504), (10769, -3.7, 4.155)]
Xr = x0.astype(np.float32).astype(np.float64)
rc, log, y = rew_run("rew_pk", x0, fs)
if rc == 0:
    ref = _sg.sosfilt(np.array([rbj(*f, fs) for f in FIVE]), Xr, axis=0)
    e = np.max(np.abs(y - ref)); rel = 10 * np.log10(np.sum((y - ref) ** 2) / np.sum(ref ** 2))
    check("PK 5 個: 出力が IIR (sosfilt) と一致 (最大差 %.1e, 相対 %.0f dB)" % (e, rel), e < 5e-5 and rel < -70)
    check("検出ログ: 有効 5 / 全 28 スロット", "有効 5 個 / 全 28" in log)
else:
    check("rew_pk の処理", False, log[-200:])
rc1, l1, y1 = rew_run("rew_disabled", x0, fs); rc2, l2, y2 = rew_run("rew_only_first", x0, fs)
check("無効 (enabled=false) のフィルタは無視される (有効だけの場合と一致)", rc1 == 0 and rc2 == 0 and np.max(np.abs(y1 - y2)) < 1e-12)
rc, log, _ = rew_run("rew_none", x0, fs); check("有効フィルタなしはエラー終了", rc != 0 and "有効なフィルタ" in log, "rc=%d" % rc)
rc, log, _ = rew_run("rew_ls", x0, fs); check("未対応種別 (LS) が有効ならエラー終了 (推測しない)", rc != 0 and "LS" in log, "rc=%d" % rc)
rc, log, y = rew_run("rew_boost", x0, fs)
if rc == 0:
    ref = _sg.sosfilt(np.array([rbj(1000, 18, 2, fs)]), Xr, axis=0)
    check("+18dB ブーストも制限・正規化なしでそのまま適用 (既定の上限 12dB を超えても)", np.max(np.abs(y - ref)) < 5e-5, "最大差 %.1e" % np.max(np.abs(y - ref)))
else:
    check("rew_boost の処理", False, log[-200:])
rng = np.random.default_rng(3); X441 = rng.standard_normal((44100 * 2, 2)) * 0.1
rc, log, y = rew_run("rew_pk", X441, 44100)
if rc == 0:
    ref = _sg.sosfilt(np.array([rbj(*f, 44100) for f in FIVE]), X441.astype(np.float32).astype(np.float64), axis=0)
    e = np.max(np.abs(y - ref)); check("44.1kHz: 係数を RBJ で再計算して適用 (最大差 %.1e)" % e, e < 5e-5 and "再計算" in log)
else:
    check("44.1kHz 処理", False, log[-200:])
rc, log, y = rew_run("rew_pk", x0, fs, "--req-role", "measurement")
if rc == 0:
    f_, Px = _sg.welch(Xr[:, 0], fs, nperseg=16384); _, Py = _sg.welch(y[:, 0], fs, nperseg=16384)
    i = np.argmin(abs(f_ - 142)); d = 10 * np.log10(Py[i - 1:i + 2].mean() / Px[i - 1:i + 2].mean())
    check("--req-role measurement: EQ のカット帯域を持ち上げる方向になる (142Hz %+.1f dB)" % d, d > 3)
else:
    check("measurement ロール", False, log[-200:])
rc, log = run(ref_in, os.path.join(tmp, "e.wav"), "--req-role", "correction")
check("--req-role correction を測定データ(テキスト .req)に使うとエラー", rc != 0 and "EQ フィルタ形式" in log, "rc=%d" % rc)

print("== クリップ処理")
loud = np.clip(x0 * 3.0, -0.99, 0.99)
write_raw(os.path.join(tmp, "loud.wav"), fs, loud, "i16")
rc, log = run(os.path.join(tmp, "loud.wav"), os.path.join(tmp, "loud0.wav"), "--gain-db", "12")
check("0dBFS 超過時にクリップ警告が出る", "クリップ" in log, log[-160:])
rc, log = run(os.path.join(tmp, "loud.wav"), os.path.join(tmp, "loud1.wav"), "--gain-db", "12", "--protect-clip")
_, y, _ = read_wav(os.path.join(tmp, "loud1.wav"))
check("--protect-clip でピーク ≦ -0.1dBFS (実測 %.4f)" % np.max(np.abs(y)), np.max(np.abs(y)) <= 0.9887 and "クリップしました" not in log)

print("== 日本語を含むパス")
jp = os.path.join(tmp, "入力 テスト.wav"); write_raw(jp, fs, x0[:20000], "i16")
rc, log = run(jp, os.path.join(tmp, "出力 結果.wav"))
check("日本語ファイル名の入出力", rc == 0 and os.path.exists(os.path.join(tmp, "出力 結果.wav")) and os.path.exists(os.path.join(tmp, "出力 結果.wav.json")), log[-160:])

print()
print("結果: %s (失敗 %d 件)" % ("全テスト合格" if not fails else "失敗あり", len(fails)))
for f in fails:
    print("  - " + f)
sys.exit(1 if fails else 0)
