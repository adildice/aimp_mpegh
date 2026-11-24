"""Compares our headphone render with a reference recording of the same song.

usage: python tools/compare_render.py ours.wav reference.wav
Both 48 kHz stereo. Prints, per third-octave band: tone difference (reference minus ours, dB,
level-matched), width (side/mid energy, dB) and left/right coherence for both.
"""
import sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import csd, welch, correlate

def load(p):
    fs, x = wavfile.read(p)
    x = x.astype(np.float64)
    if x.dtype.kind == "i":
        x /= np.iinfo(x.dtype).max
    return fs, x

def align(a, b, fs):
    # offset of b against a, from the first 60 s of mono, coarse (decimated) then fine
    n = min(len(a), len(b), fs * 60)
    ma, mb = a[:n].mean(1), b[:n].mean(1)
    c = correlate(mb[::8], ma[::8], mode="full", method="fft")
    lag = (np.argmax(np.abs(c)) - (len(ma[::8]) - 1)) * 8
    w = slice(fs * 10, fs * 20)
    best, bl = -1, lag
    for l in range(lag - 16, lag + 17):
        seg_b = mb[w.start + l: w.stop + l] if w.start + l >= 0 else None
        if seg_b is None or len(seg_b) != w.stop - w.start:
            continue
        r = np.dot(ma[w], seg_b)
        if r > best:
            best, bl = r, l
    return bl

def bands():
    f = 1000.0 * 2.0 ** (np.arange(-17, 14) / 3.0)  # 20 Hz .. 20 kHz
    return f[(f >= 25) & (f <= 18000)]

def analyse(x, fs):
    L, R = x[:, 0], x[:, 1]
    M, S = (L + R) / 2, (L - R) / 2
    nper = 8192
    f, pm = welch(M, fs, nperseg=nper)
    _, ps = welch(S, fs, nperseg=nper)
    _, pl = welch(L, fs, nperseg=nper)
    _, pr = welch(R, fs, nperseg=nper)
    _, plr = csd(L, R, fs, nperseg=nper)
    out = []
    for fc in bands():
        k = (f >= fc / 2 ** (1 / 6)) & (f < fc * 2 ** (1 / 6))
        mid = pm[k].sum() + 1e-20
        out.append((fc, 10 * np.log10(mid), 10 * np.log10((ps[k].sum() + 1e-20) / mid),
                    np.abs(plr[k].sum()) / np.sqrt(pl[k].sum() * pr[k].sum() + 1e-30)))
    return out

def main():
    fs, a = load(sys.argv[1])
    fs2, b = load(sys.argv[2])
    assert fs == fs2 == 48000
    lag = align(a, b, fs)
    if lag > 0:
        b = b[lag:]
    else:
        a = a[-lag:]
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    ra, rb = analyse(a, fs), analyse(b, fs)
    # level-match tone curves at 500 Hz - 2 kHz
    ref = [i for i, r in enumerate(ra) if 500 <= r[0] <= 2000]
    off = np.mean([rb[i][1] - ra[i][1] for i in ref])
    print(f"offset {lag} samples ({lag / fs * 1000:.1f} ms), compared {n / fs:.0f} s")
    print(" band Hz | tone ref-ours dB | width ours / ref dB | coherence ours / ref")
    for (fc, ta, wa, ca), (_, tb, wb, cb) in zip(ra, rb):
        print(f"{fc:8.0f} | {tb - ta - off:+6.1f}           | {wa:+6.1f} / {wb:+6.1f}      | {ca:.2f} / {cb:.2f}")

if __name__ == "__main__":
    main()
