"""検証用の最小 WAV 入出力 (numpy のみ)。C++ 側の読み書きとは独立に実装している。"""
import struct
import numpy as np


def read_wav(path):
    b = open(path, "rb").read()
    assert b[:4] == b"RIFF" and b[8:12] == b"WAVE", "not RIFF/WAVE"
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid, sz = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        body = b[pos + 8:pos + 8 + sz]
        if cid == b"fmt ":
            tag, ch, fs, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, fs, bits)
        elif cid == b"data":
            data = body
            break
        pos += 8 + sz + (sz & 1)
    tag, ch, fs, bits = fmt
    if tag == 3:
        x = np.frombuffer(data, dtype="<f4" if bits == 32 else "<f8").astype(np.float64)
    elif bits == 16:
        x = np.frombuffer(data, dtype="<i2").astype(np.float64) / 32768.0
    elif bits == 24:
        a = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = a[:, 0] | (a[:, 1] << 8) | (a[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v.astype(np.float64) / 8388608.0
    elif bits == 32:
        x = np.frombuffer(data, dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        raise ValueError("unsupported")
    return fs, x.reshape(-1, ch), (tag, bits)


def write_wav(path, fs, x, bits=24):
    """x: (frames, ch) in [-1,1). PCM 16/24/32 を書く (標準 WAVE_FORMAT_PCM ヘッダ)"""
    x = np.atleast_2d(x.T).T
    n, ch = x.shape
    scale = float(2 ** (bits - 1))
    iv = np.clip(np.round(x * scale), -scale, scale - 1).astype(np.int64)
    if bits == 16:
        raw = iv.astype("<i2").tobytes()
    elif bits == 24:
        u = (iv & 0xFFFFFF).astype(np.uint32)
        raw = np.stack([(u & 0xFF), (u >> 8) & 0xFF, (u >> 16) & 0xFF], axis=-1).astype(np.uint8).tobytes()
    else:
        raw = iv.astype("<i4").tobytes()
    hdr = b"RIFF" + struct.pack("<I", 36 + len(raw)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, ch, fs, fs * ch * bits // 8, ch * bits // 8, bits) + b"data" + struct.pack("<I", len(raw))
    open(path, "wb").write(hdr + raw)
