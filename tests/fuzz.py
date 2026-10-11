#!/usr/bin/env python3
"""ファジング (壊れた入力でクラッシュ/未定義動作が出ないか)。ASan+UBSan ビルドが必要:

  g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -mavx2 -o eq_san src/main.cpp
  python3 tests/fuzz.py ./eq_san [件数/種類]
.req (Java 形式の切り詰め・書き換え)、WAV ヘッダ、テキスト .req を壊して実行し、
サニタイザの検出・異常終了・不正UTF-8出力があれば失敗にする。"""
import os, random, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import numpy as np
from wavio import write_wav

exe = sys.argv[1] if len(sys.argv) > 1 else "./eq_san"
n_each = int(sys.argv[2]) if len(sys.argv) > 2 else 300
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
data = os.path.join(root, "tests", "data")
tmp = tempfile.mkdtemp()
random.seed(2026)
write_wav(os.path.join(tmp, "small.wav"), 48000, np.random.default_rng(1).standard_normal((3000, 2)) * 0.1, 16)
small = os.path.join(tmp, "small.wav")
bad = 0; total = 0


def run(args):
    global bad, total
    p = subprocess.run([exe] + args, capture_output=True, timeout=120)
    total += 1
    out = p.stdout + p.stderr
    problem = None
    if b"Sanitizer" in out or b"runtime error" in out: problem = "サニタイザ検出"
    elif p.returncode not in (0, 1, 2, 3, 4, 5): problem = "異常終了 rc=%d" % p.returncode
    else:
        try: out.decode("utf-8")
        except UnicodeDecodeError: problem = "不正UTF-8を出力"
    if problem:
        bad += 1; print("  NG:", problem, args[-3:], out[-300:])


def mutate(b, mode):
    b = bytearray(b)
    if mode == 0: return b[:random.randint(4, len(b) - 1)]
    if mode == 1:
        for _ in range(random.randint(1, 8)): b[random.randint(0, len(b) - 1)] = random.randint(0, 255)
    else:
        pos = random.randint(0, min(400, len(b) - 1)); b[pos:pos + random.randint(1, 6)] = os.urandom(random.randint(1, 6))
    return b


fz = os.path.join(tmp, "fz.bin")
for name in ["java_arrays", "java_hashmap", "java_wrapped", "java_points", "java_lists", "java_matrix", "rew_pk", "rew_disabled"]:
    src = open(os.path.join(data, name + ".req"), "rb").read()
    for i in range(n_each):
        open(fz, "wb").write(mutate(src, i % 3)); run(["--dump-req", fz])
src = open(small, "rb").read()
for i in range(n_each * 3):
    b = bytearray(src)
    for _ in range(random.randint(1, 6)): b[random.randint(0, 80)] = random.randint(0, 255)
    if i % 4 == 0: b = b[:random.randint(1, len(b) - 1)]
    open(fz, "wb").write(b)
    run([fz, os.path.join(data, "eq_text.req"), "-o", os.path.join(tmp, "o.wav"), "-j", os.path.join(tmp, "o.json"), "--taps", "2048"])
txt = open(os.path.join(data, "eq_text.req"), "rb").read()
for i in range(n_each * 2):
    b = bytearray(txt)
    for _ in range(random.randint(1, 30)): b[random.randint(0, len(b) - 1)] = random.choice(b"0123456789.-e+,;\t \n#abc")
    open(fz, "wb").write(b)
    run([small, fz, "-o", os.path.join(tmp, "o.wav"), "-j", os.path.join(tmp, "o.json"), "--taps", "2048"])
# マルチチャンネル: 壊れた 6ch WAV (チャンネルマスク/チャンネル数の破損) × フォルダ指定 / NAME= / --channels の乱れ
mc = os.path.join(data, "mc")
if os.path.isdir(mc):
    import struct
    X = (np.random.default_rng(2).standard_normal((3000, 6)) * 0.1 * 2147483648).astype("<i4").tobytes()
    fmt = struct.pack("<HHIIHH", 0xFFFE, 6, 48000, 48000 * 24, 24, 32) + struct.pack("<HHI", 22, 32, 0x3F) + struct.pack("<H", 1) + bytes.fromhex("000000001000800000AA00389B71")
    base = b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(X)) + b"WAVEfmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(X)) + X
    names = ["L", "R", "C", "LFE", "Lrs", "Rrs", "Ls", "SW", "Foo", "", "l", "Lss"]
    for i in range(n_each * 3):
        b = bytearray(base)
        for _ in range(random.randint(1, 4)): b[random.randint(12, 80)] = random.randint(0, 255)     # fmt 部 (nch, mask など) を壊す
        open(fz, "wb").write(b)
        args = [fz]
        r = i % 4
        if r == 0: args += [mc]
        elif r == 1: args += ["%s=%s" % (random.choice(names), os.path.join(mc, "null_EQ_%s.req" % random.choice(["L", "C", "Rrs"]))) for _ in range(random.randint(1, 4))]
        elif r == 2: args += [mc, "--channels", ",".join(random.choice(names) for _ in range(random.randint(1, 9)))]
        else: args += [os.path.join(mc, "null_EQ_C.req"), os.path.join(mc, "null_EQ_L.req"), "--missing", random.choice(["error", "passthrough", "x"])]
        run(args + ["-o", os.path.join(tmp, "o.wav"), "-j", os.path.join(tmp, "o.json"), "--taps", "2048"])
print("ファジング %d 件, 問題 %d 件" % (total, bad))
sys.exit(1 if bad else 0)
