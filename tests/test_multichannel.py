#!/usr/bin/env python3
"""マルチチャンネル (チャンネルごとの .req) の回帰テスト。終了コード 0 = 全合格
  python3 tests/test_multichannel.py [eqcorrect実行ファイル]
前提: tests/data/mc/ に MakeRew の出力 (null_EQ_{L,R,C,LFE,Lrs,Rrs}.req) があること"""
import json, os, shutil, struct, subprocess, sys, tempfile
import numpy as np
from scipy import signal as sg
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from wavio import read_wav

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "eqcorrect")
MC = os.path.join(ROOT, "tests", "data", "mc")
tmp = tempfile.mkdtemp()
fails = []
FS = 48000


def check(name, cond, detail=""):
    print(("  PASS " if cond else "  FAIL ") + name + (("  - " + detail) if (detail and not cond) else ""))
    if not cond:
        fails.append(name)


def write_wav(path, x, mask=None, kind="i32"):
    """x: (frames, ch)。mask=None なら通常の PCM ヘッダ、整数なら WAVE_FORMAT_EXTENSIBLE + dwChannelMask"""
    n, ch = x.shape
    if kind == "i32":
        d = np.clip(np.round(x * 2147483648), -2147483648, 2147483647).astype("<i4").tobytes(); bits, tag = 32, 1
    else:
        d = x.astype("<f4").tobytes(); bits, tag = 32, 3
    if mask is None:
        fmt = struct.pack("<HHIIHH", tag, ch, FS, FS * ch * bits // 8, ch * bits // 8, bits)
    else:
        fmt = struct.pack("<HHIIHH", 0xFFFE, ch, FS, FS * ch * bits // 8, ch * bits // 8, bits) + struct.pack("<HHI", 22, bits, mask) \
            + struct.pack("<H", tag) + bytes.fromhex("000000001000800000AA00389B71")
    body = b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(d)) + d
    open(path, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)


def read_mask(path):
    b = open(path, "rb").read()
    pos = 12
    while pos + 8 <= len(b):
        cid, sz = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        if cid == b"fmt ":
            tag = struct.unpack("<H", b[pos + 8:pos + 10])[0]
            return struct.unpack("<I", b[pos + 28:pos + 32])[0] if tag == 0xFFFE else None
        pos += 8 + sz + (sz & 1)


def run(*args):
    p = subprocess.run([EXE, *args], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def rbj(fc, g, q):
    A = 10 ** (g / 40); w0 = 2 * np.pi * fc / FS; al = np.sin(w0) / (2 * q); c = np.cos(w0); a0 = 1 + al / A
    return [(1 + al * A) / a0, -2 * c / a0, (1 - al * A) / a0, 1, -2 * c / a0, (1 - al / A) / a0]


# MakeRew.java と同じ内容 (独立な IIR 基準用)
SPEC = {
    "L": [(100, -6, 2), (3000, 3, 1.5)], "R": [(120, -4, 3)],
    "C": [(57.5, -10.1, 2.539), (86.5, -8.2, 4.299), (142, -11.3, 1.599), (277, -6.1, 1.504), (10769, -3.7, 4.155)],
    "LFE": [(40, -8, 2), (80, 4, 3)], "Lrs": [(500, -5, 1)], "Rrs": [(700, 5, 1)],
}
ORDER6 = ["L", "R", "C", "LFE", "Lrs", "Rrs"]      # 5.1 (マスク 0x3F) の並び
rng = np.random.default_rng(11)
N = FS * 2
X6 = rng.standard_normal((N, 6)) * 0.05 * np.array([1, 0.8, 1.2, 0.6, 0.9, 1.1])
X6 = np.round(X6 * 2147483648) / 2147483648        # i32 で正確に表現できる値
req = lambda n: os.path.join(MC, "null_EQ_%s.req" % n)

print("== 基本: 5.1 (マスク 0x3F) にフォルダ指定")
w6 = os.path.join(tmp, "mc6.wav"); write_wav(w6, X6, 0x3F)
out = os.path.join(tmp, "o6.wav"); js = out + ".json"
rc, log = run(w6, MC, "-o", out, "-j", js, "--bits", "32", "--dither", "off")
check("フォルダ指定で処理が成功 (rc=%d)" % rc, rc == 0, log[-300:])
if rc == 0:
    _, Y, _ = read_wav(out)
    # (1) 独立基準: チャンネルごとの IIR (RBJ バイクアッド; scipy sosfilt)
    errs = []
    for i, nm in enumerate(ORDER6):
        ref = sg.sosfilt(np.array([rbj(*f) for f in SPEC[nm]]), X6[:, i])
        errs.append(np.max(np.abs(Y[:, i] - ref)))
    check("全チャンネルが各自の IIR 基準と一致 (最大差 %s)" % ", ".join("%.1e" % e for e in errs), max(errs) < 5e-5)
    # (2) 各チャンネルを単独 (モノラル) で処理した結果と一致 → ペア詰め(別フィルタ分離)が正しい
    worst = 0
    for i, nm in enumerate(ORDER6):
        wm = os.path.join(tmp, "m_%s.wav" % nm); write_wav(wm, X6[:, i:i + 1])
        om = os.path.join(tmp, "m_%s_o.wav" % nm)
        r, l = run(wm, req(nm), "-o", om, "-j", om + ".json", "--bits", "32", "--dither", "off")
        if r != 0:
            check("単独処理 %s" % nm, False, l[-200:]); continue
        worst = max(worst, np.max(np.abs(read_wav(om)[1][:, 0] - Y[:, i])))
    check("各チャンネルの結果が単独処理と一致 (最大差 %.1e)" % worst, worst < 2e-9)
    check("出力にチャンネルマスク 0x3F が保持される", read_mask(out) == 0x3F, str(read_mask(out)))
    J = json.load(open(js))
    ci = J["channel_info"]
    check("JSON channel_info: 名前の並びと割り当て", [c["name"] for c in ci] == ORDER6 and all(c["corrected"] for c in ci)
          and [J["designs"][c["design"]]["req_file"] for c in ci] == ["null_EQ_%s.req" % n for n in ORDER6])
    check("JSON: 設計 6 件・スペクトル 6 ch・名前つき", len(J["designs"]) == 6 and len(J["spectrum"]) == 6 and J["spectrum"][3]["name"] == "LFE")
    # 実測伝達関数 (Welch) が設計と一致しているか: LFE の 80Hz 付近は +4dB 近辺のピーク
    g = np.array(J["grid"]); h = np.array(J["spectrum"][3]["h_db"], dtype=float)
    k = np.argmin(abs(g - 80)); check("LFE の実測伝達関数に 80Hz のブースト (%+.1f dB)" % h[k], h[k] > 2.5)

print("== 同じフィルタの組 / 別フィルタの組 / 奇数チャンネル (組の作り方が違う経路)")
# 3ch: L,R,C (奇数 → 余りは単独処理)。7.1 のサブセット
w3 = os.path.join(tmp, "mc3.wav"); write_wav(w3, X6[:, :3], 0x7)
o3 = os.path.join(tmp, "o3.wav")
rc, log = run(w3, req("L"), req("R"), req("C"), "-o", o3, "-j", o3 + ".json", "--bits", "32", "--dither", "off")
if rc == 0:
    Y3 = read_wav(o3)[1]
    d = max(np.max(np.abs(Y3[:, i] - read_wav(os.path.join(tmp, "m_%s_o.wav" % nm))[1][:, 0])) for i, nm in enumerate(["L", "R", "C"]))
    check("3ch (奇数) も単独処理と一致 (最大差 %.1e)" % d, d < 2e-9)
else:
    check("3ch 処理", False, log[-300:])
# 全チャンネル同じ .req (従来動作) と、NAME= で同じファイルを各チャンネルへ
same = os.path.join(tmp, "same.wav")
rc, log = run(w6, req("C"), "-o", same, "-j", same + ".json", "--bits", "32", "--dither", "off")
check("従来動作: .req 1 つ → 全チャンネルに適用 (rc=%d)" % rc, rc == 0 and "全 6 チャンネルに" in log)
if rc == 0:
    Ys = read_wav(same)[1]
    d = max(np.max(np.abs(Ys[:, i] - sg.sosfilt(np.array([rbj(*f) for f in SPEC["C"]]), X6[:, i]))) for i in range(6))
    check("  全チャンネルが C のIIRと一致 (最大差 %.1e)" % d, d < 5e-5)
    check("  ファイル名が C を示す旨の注意が出る", "ファイル名はチャンネル C を示して" in log)
o_eq = os.path.join(tmp, "oeq.wav")
rc, log = run(w6, "L=" + req("C"), "R=" + req("C"), "C=" + req("C"), "LFE=" + req("C"), "Lrs=" + req("C"), "Rrs=" + req("C"),
              "-o", o_eq, "-j", o_eq + ".json", "--bits", "32", "--dither", "off")
check("NAME=ファイル を全チャンネルに同じファイルで指定 → 従来動作と一致", rc == 0 and np.max(np.abs(read_wav(o_eq)[1] - read_wav(same)[1])) < 2e-9, log[-200:])
if rc == 0:
    check("  同じ .req は設計 1 件にまとめられる", len(json.load(open(o_eq + ".json"))["designs"]) == 1)

print("== 割り当てのルール")
# 一部だけ → 他は素通し (ビット単位で入力と同一)
op = os.path.join(tmp, "op.wav")
rc, log = run(w6, req("L"), req("C"), "-o", op, "-j", op + ".json", "--bits", "32", "--dither", "off")
check("一部のチャンネルだけ .req (rc=%d)" % rc, rc == 0 and "補正せずそのまま通します: R, LFE, Lrs, Rrs" in log, log[-300:])
if rc == 0:
    Yp = read_wav(op)[1]
    check("  .req の無いチャンネルは入力と完全に同一", all(np.array_equal(Yp[:, i], X6[:, i]) for i in (1, 3, 4, 5)))
    check("  .req のあるチャンネルは補正される", all(np.max(np.abs(Yp[:, i] - X6[:, i])) > 1e-3 for i in (0, 2)))
    ci = json.load(open(op + ".json"))["channel_info"]
    check("  JSON: corrected フラグ", [c["corrected"] for c in ci] == [True, False, True, False, False, False])
rc, log = run(w6, req("L"), req("C"), "--missing", "error", "-o", op)
check("--missing error: .req が無いチャンネルがあるとエラー", rc != 0 and "R, LFE, Lrs, Rrs" in log)
# 判定できない名前 / 重複 / 存在しないチャンネル
shutil.copy(req("L"), os.path.join(tmp, "foo.req"))
rc, log = run(w6, req("L"), os.path.join(tmp, "foo.req"), "-o", op)
check("チャンネルを判定できないファイル名はエラー (推測しない)", rc != 0 and "判定できません" in log and "NAME=" in log)
rc, log = run(w6, req("L"), "L=" + req("R"), "-o", op)
check("同じチャンネルに 2 つの .req はエラー", rc != 0 and "2 つの .req" in log)
w2 = os.path.join(tmp, "st.wav"); write_wav(w2, X6[:, :2], 0x3)
rc, log = run(w2, req("L"), req("Lrs"), "-o", op)
check("WAV に無いチャンネル (ステレオに Lrs) はエラー", rc != 0 and "ありません" in log and "--ignore-extra" in log)
rc, log = run(w2, req("L"), req("Lrs"), "-o", op, "-j", op + ".json")
rc2, log2 = run(w2, req("L"), req("Lrs"), "--ignore-extra", "-o", op, "-j", op + ".json")
check("--ignore-extra で無視して処理", rc2 == 0 and "使いません" in log2, log2[-200:])
rc, log = run(w2, MC, "-o", op, "-j", op + ".json")
check("フォルダ指定ならステレオでも L,R だけ使って処理 (余分は通知して無視)", rc == 0 and log.count("使いません") == 4, log[-300:])
rc, log = run(w6, "XYZ=" + req("L"), "-o", op)
check("存在しない NAME は「ファイルが開けない」エラーになる (NAME と解釈されない)", rc != 0)

print("== チャンネル配置 (マスク / 標準配置 / --channels / サラウンド別名)")
# マスクなし 6ch → 5.1 を仮定してログに出す
wn = os.path.join(tmp, "nomask.wav"); write_wav(wn, X6)
on = os.path.join(tmp, "on.wav")
rc, log = run(wn, MC, "-o", on, "-j", on + ".json", "--bits", "32", "--dither", "off")
check("マスク無し 6ch → 標準 5.1 配置を仮定 (ログに明示)", rc == 0 and "チャンネルマスクが無いため" in log and "1:L 2:R 3:C 4:LFE 5:Lrs 6:Rrs" in log, log[-300:])
if rc == 0:
    check("  マスク無し入力の結果はマスク付きと同一", np.max(np.abs(read_wav(on)[1] - read_wav(out)[1])) == 0)
# --channels で並べ替え: 先頭を C にする
wc = os.path.join(tmp, "reorder.wav"); Xr = X6[:, [2, 0, 1, 3, 4, 5]]; write_wav(wc, Xr, None)
oc = os.path.join(tmp, "oc.wav")
rc, log = run(wc, MC, "--channels", "C,L,R,LFE,Lrs,Rrs", "-o", oc, "-j", oc + ".json", "--bits", "32", "--dither", "off")
if rc == 0:
    Yc = read_wav(oc)[1]
    check("--channels で並びを指定 → 並べ替え前と同じ補正", np.max(np.abs(Yc - read_wav(out)[1][:, [2, 0, 1, 3, 4, 5]])) < 2e-9)
else:
    check("--channels 処理", False, log[-300:])
rc, log = run(wc, MC, "--channels", "C,L,R", "-o", oc); check("--channels の個数不一致はエラー", rc != 0 and "一致しません" in log)
rc, log = run(wc, MC, "--channels", "C,L,R,LFE,Lrs,Foo", "-o", oc); check("--channels の未知の名前はエラー", rc != 0 and "Foo" in log)
rc, log = run(wc, MC, "--channels", "C,L,R,LFE,Lrs,Lrs", "-o", oc); check("--channels の重複はエラー", rc != 0 and "重複" in log)
# 5.1 side (0x60F: FL FR FC LFE SL SR) に Lrs/Rrs の .req → サイドへ読み替え (通知あり)
w60f = os.path.join(tmp, "side.wav"); write_wav(w60f, X6, 0x60F)
o60 = os.path.join(tmp, "o60.wav")
rc, log = run(w60f, MC, "-o", o60, "-j", o60 + ".json", "--bits", "32", "--dither", "off")
check("5.1(サイド配置 0x60F) に Lrs/Rrs の .req → サイドとして扱う (通知あり)", rc == 0 and "Lrs は Lss として扱います" in log and "1:L 2:R 3:C 4:LFE 5:Lss 6:Rss" in log, log[-400:])
if rc == 0:
    check("  結果は 0x3F 版と同一 / 出力マスクは 0x60F を保持", np.max(np.abs(read_wav(o60)[1] - read_wav(out)[1])) == 0 and read_mask(o60) == 0x60F)
# 7.1 (0x63F) では Lrs/Rrs はリア、Lss/Rss はサイドで別チャンネル
X8 = rng.standard_normal((FS, 8)) * 0.05; X8 = np.round(X8 * 2147483648) / 2147483648
w8 = os.path.join(tmp, "mc8.wav"); write_wav(w8, X8, 0x63F)
o8 = os.path.join(tmp, "o8.wav")
rc, log = run(w8, req("Lrs"), "Lss=" + req("Rrs"), "-o", o8, "-j", o8 + ".json", "--bits", "32", "--dither", "off")
check("7.1: 並び = L R C LFE Lrs Rrs Lss Rss", rc == 0 and "1:L 2:R 3:C 4:LFE 5:Lrs 6:Rrs 7:Lss 8:Rss" in log, log[-300:])
if rc == 0:
    Y8 = read_wav(o8)[1]
    check("  Lrs(5ch目) と Lss(7ch目) だけ補正され、他は素通し", all(np.max(np.abs(Y8[:, i] - X8[:, i])) > 1e-3 for i in (4, 6)) and all(np.array_equal(Y8[:, i], X8[:, i]) for i in (0, 1, 2, 3, 5, 7)))
    check("  7.1 のマスク 0x63F を保持", read_mask(o8) == 0x63F)
# 10ch: 標準配置が無い → 従来動作(全 ch 同じ .req)は可、チャンネル別は --channels 必須
X10 = rng.standard_normal((FS // 2, 10)) * 0.05
w10 = os.path.join(tmp, "mc10.wav"); write_wav(w10, X10, None, "f32")
o10 = os.path.join(tmp, "o10.wav")
rc, log = run(w10, req("C"), "-o", o10, "-j", o10 + ".json")
check("10ch + .req 1 つ(従来動作)は配置不明でも動く", rc == 0 and "全 10 チャンネルに" in log, log[-200:])
rc, log = run(w10, MC, "-o", o10)
check("10ch でチャンネル別 .req は --channels 無しだとエラー (推測しない)", rc != 0 and "--channels" in log)
rc, log = run(w10, MC, "--channels", "L,R,C,LFE,Lrs,Rrs,Lss,Rss,Lc,Rc", "-o", o10, "-j", o10 + ".json")
check("10ch + --channels 指定で処理 (残り 4ch は素通し)", rc == 0 and "補正せずそのまま通します: Lss, Rss, Lc, Rc" in log, log[-200:])
# 16bit 出力でもマスク保持、--save-filter はチャンネル別ファイル
o16 = os.path.join(tmp, "o16.wav"); sf = os.path.join(tmp, "flt.wav")
rc, log = run(w6, MC, "-o", o16, "-j", o16 + ".json", "--save-filter", sf)
check("16bit 出力でもマスク保持 / --save-filter はチャンネル名つきで保存", rc == 0 and read_mask(o16) == 0x3F and all(os.path.exists(os.path.join(tmp, "flt_%s.wav" % n)) for n in ORDER6), log[-200:])

print("== 日本語パス・フォルダ")
jd = os.path.join(tmp, "測定 データ"); shutil.copytree(MC, jd)
rc, log = run(w6, jd, "-o", os.path.join(tmp, "日本語 出力.wav"), "-j", os.path.join(tmp, "日本語.json"))
check("日本語を含むフォルダ・出力パス", rc == 0 and os.path.exists(os.path.join(tmp, "日本語 出力.wav")), log[-200:])
rc, log = run(w6, os.path.join(tmp, "空フォルダなし"), "-o", op)
check("存在しない .req はエラー", rc != 0)
os.makedirs(os.path.join(tmp, "empty")); rc, log = run(w6, os.path.join(tmp, "empty"), "-o", op)
check("空フォルダはエラー", rc != 0 and ".req ファイルがありません" in log)

print("結果: %s" % ("全テスト合格 (失敗 0 件)" if not fails else "失敗 %d 件: %s" % (len(fails), ", ".join(fails))))
shutil.rmtree(tmp, ignore_errors=True)
sys.exit(1 if fails else 0)
