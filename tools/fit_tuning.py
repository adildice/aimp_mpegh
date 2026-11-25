"""Fits the Headphones (3D) tone correction against reference recordings of several songs.

usage: python tools/fit_tuning.py ours1.wav ref1.wav [ours2.wav ref2.wav ...]
  oursN.wav - the plugin's current Headphones (3D) render of a song (test_host.exe output)
  refN.wav  - a recording of the same song from Amazon Music's 360 Reality Audio, 48 kHz

Prints each song's remaining tone/width difference and the change to the EQ in
src/binaural.h (the eq[][] table and m_side) that best fits all songs together.
A change every song agrees on is the renderer's; one only a single song wants is that mix's.
"""
import os
import sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scipy.signal import lfilter
from scipy.optimize import minimize
from compare_render import load, align, analyse

FS = 48000
SIMPLEX = np.vstack([np.zeros(5), np.eye(5)])  # first steps of 1 dB per gain
# current EQ in src/binaural.h: (kind, Hz, Q); gains are fitted as changes from the current ones
BANDS = [("peak", 90, 0.9), ("low", 300, 0.7), ("high", 3000, 0.7), ("high", 11000, 0.7)]
SIDE = ("high", 1800, 0.7)

def rbj(kind, f0, gdb, q, fs=FS):
    A = 10 ** (gdb / 40); w = 2 * np.pi * f0 / fs; cw, sw = np.cos(w), np.sin(w)
    al = sw / (2 * q); s = 2 * np.sqrt(A) * al
    if kind == "peak":
        b = [1 + al * A, -2 * cw, 1 - al * A]; a = [1 + al / A, -2 * cw, 1 - al / A]
    elif kind == "low":
        b = [A * ((A + 1) - (A - 1) * cw + s), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - s)]
        a = [(A + 1) + (A - 1) * cw + s, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - s]
    else:
        b = [A * ((A + 1) + (A - 1) * cw + s), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - s)]
        a = [(A + 1) - (A - 1) * cw + s, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - s]
    return np.array(b) / a[0], np.array(a) / a[0]

def apply(p, x):
    y = x.copy()
    for (kind, f0, q), g in zip(BANDS, p[:4]):
        if abs(g) > 1e-3:
            b, a = rbj(kind, f0, g, q)
            y = lfilter(b, a, y, axis=0)
    M, S = (y[:, 0] + y[:, 1]) / 2, (y[:, 0] - y[:, 1]) / 2
    if abs(p[4]) > 1e-3:
        b, a = rbj(SIDE[0], SIDE[1], p[4], SIDE[2])
        S = lfilter(b, a, S)
    return np.stack([M + S, M - S], 1)

def errors(ra, rb):
    ref = [i for i, r in enumerate(ra) if 500 <= r[0] <= 2000]
    off = np.mean([ra[i][1] - rb[i][1] for i in ref])
    t = np.array([ra[i][1] - rb[i][1] - off for i in range(len(ra)) if 40 <= ra[i][0] <= 16000])
    w = np.array([ra[i][2] - rb[i][2] for i in range(len(ra)) if 1000 <= ra[i][0] <= 16000])
    return t, w

CHUNK = 5  # seconds

def has_dropout(x, fs):
    # a buffering gap shows up as a stretch of near-silence inside the piece
    m = np.abs(x).mean(1)
    win = fs // 20
    env = np.convolve(m, np.ones(win) / win, mode="valid")[::win // 2]
    return env.min() < env.mean() * 0.003

def prepare(ours_path, ref_path):
    """Lines up the reference piece by piece, so dropouts in a streamed recording only cost
    the pieces they fall in. Returns (our matched audio, analysis of the matched reference)."""
    fs, a = load(ours_path)
    fs2, b = load(ref_path)
    assert fs == fs2 == FS, "both files must be 48 kHz"
    from scipy.signal import correlate
    D = 8
    am = a.mean(1)[::D]
    n = CHUNK * fs
    ours_parts, ref_parts, kept, total = [], [], 0, 0
    for s in range(0, len(b) - n, n):
        piece = b[s:s + n]
        total += 1
        if has_dropout(piece, fs):
            continue
        pm = piece.mean(1)[::D]
        c = correlate(am, pm, mode="valid", method="fft")
        k = int(np.argmax(c))
        seg = am[k:k + len(pm)]
        r = np.dot(seg, pm) / (np.linalg.norm(seg) * np.linalg.norm(pm) + 1e-12)
        if r < 0.6:  # not found reliably (noise, dropout, different version)
            continue
        start = k * D
        if start + n > len(a):
            continue
        ours_parts.append(a[start:start + n])
        ref_parts.append(piece)
        kept += 1
    print(f"{os.path.basename(ref_path)}: used {kept} of {total} pieces of {CHUNK} s")
    assert kept >= 4, "too few matching pieces: is it the same song and the same version?"
    # at most 12 pieces (60 s), spread over the song, keeps fitting fast
    pick = np.linspace(0, kept - 1, min(kept, 12)).round().astype(int)
    ours = np.concatenate([ours_parts[i] for i in pick])
    ref = np.concatenate([ref_parts[i] for i in pick])
    return ours, analyse(ref, fs)

def main():
    args = sys.argv[1:]
    assert args and len(args) % 2 == 0, __doc__
    songs = [prepare(args[i], args[i + 1]) for i in range(0, len(args), 2)]

    def total(p):
        e = 0
        for ours, ra in songs:
            t, w = errors(ra, analyse(apply(p, ours), FS))
            e += np.sqrt(np.mean(t ** 2)) + 0.5 * np.sqrt(np.mean(w ** 2))
        return e / len(songs)

    def report(p, title):
        print(title)
        for k, (ours, ra) in enumerate(songs):
            t, w = errors(ra, analyse(apply(p, ours), FS))
            print(f"  song {k + 1}: tone {np.sqrt(np.mean(t ** 2)):.2f} dB rms, width {np.sqrt(np.mean(w ** 2)):.2f} dB rms")

    zero = np.zeros(5)
    report(zero, "now:")
    r = minimize(total, zero, method="Nelder-Mead", options={"maxiter": 200, "xatol": 0.1, "fatol": 0.01, "initial_simplex": SIMPLEX})
    report(r.x, "with the fitted change:")
    names = [f"{k} {f} Hz" for k, f, _ in BANDS] + ["side shelf 1800 Hz"]
    print("change to add (dB):")
    for n, g in zip(names, r.x):
        print(f"  {n:22s} {g:+.1f}")
    # per-song fits show whether the songs agree
    print("per-song best change (dB):")
    for k, (ours, ra) in enumerate(songs):
        def one(p, ours=ours, ra=ra):
            t, w = errors(ra, analyse(apply(p, ours), FS))
            return np.sqrt(np.mean(t ** 2)) + 0.5 * np.sqrt(np.mean(w ** 2))
        rk = minimize(one, zero, method="Nelder-Mead", options={"maxiter": 150, "xatol": 0.1, "fatol": 0.01, "initial_simplex": SIMPLEX})
        print(f"  song {k + 1}: " + "  ".join(f"{g:+.1f}" for g in rk.x))

if __name__ == "__main__":
    main()
