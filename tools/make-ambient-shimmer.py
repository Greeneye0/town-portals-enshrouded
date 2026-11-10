"""Five more ambient loops in the manner of 3-shimmer, quieter and softer (user, 2026-11-10:
"like the shimmer but even more subtle"). 16 s, seamless, all at RMS -38 dBFS (the first six are
at -33, so these play about half as loud).

Run: python -I tools/make-ambient-shimmer.py  ->  tools/ambient-samples/7..11-*.wav
"""
from pathlib import Path
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, sosfiltfilt

RATE = 48000
SECONDS = 16
N = RATE * SECONDS
t = np.arange(N) / RATE
OUT = Path(__file__).resolve().parent / 'ambient-samples'
rng = np.random.default_rng(11)


def f(hz):
    return max(1, round(hz * SECONDS)) / SECONDS


def tone(hz, phase=0.0):
    return np.sin(2 * np.pi * f(hz) * t + phase)


def lfo(hz, lo, hi, phase=0.0):
    return lo + (hi - lo) * (0.5 + 0.5 * np.sin(2 * np.pi * f(hz) * t + phase))


def band(x, lo, hi):
    sos = butter(2, [lo, hi], btype='band', fs=RATE, output='sos')
    return sosfiltfilt(sos, np.concatenate([x, x, x]))[N:2 * N]


def noise():
    spec = np.fft.rfft(rng.standard_normal(N))
    fr = np.fft.rfftfreq(N, 1 / RATE)
    fr[0] = 1
    x = np.fft.irfft(spec / np.sqrt(fr), N)
    return x / np.max(np.abs(x))


def grains(count, lo, hi, length):
    """Soft little sine pings placed around the loop (wrapping at the end)."""
    x = np.zeros(N)
    n = int(length * RATE)
    env = np.sin(np.pi * np.arange(n) / n) ** 2
    for _ in range(count):
        at = rng.integers(0, N)
        hz = rng.uniform(lo, hi)
        g = np.sin(2 * np.pi * hz * np.arange(n) / RATE) * env * rng.uniform(0.4, 1.0)
        idx = (at + np.arange(n)) % N
        x[idx] += g
    return x


def save(name, mono, width=0.5):
    mono = mono - np.mean(mono)
    shift = int(RATE * 0.013)
    out = np.stack([mono, np.roll(mono, shift) * (1 - width) + mono * width], axis=1)
    out *= 10 ** (-38 / 20) / np.sqrt(np.mean(out ** 2))
    out *= min(1.0, 10 ** (-22 / 20) / np.max(np.abs(out)))
    wavfile.write(OUT / name, RATE, (out * 32767).astype(np.int16))
    print(name, f'peak {20 * np.log10(np.max(np.abs(out))):.1f} dBFS')


# 7. Whisper: no drone at all, only faint glassy overtones drifting in and out.
x = np.zeros(N)
for i, (hz, rate) in enumerate([(1174.7, 0.0625), (1568.0, 0.125), (1760.0, 0.1875), (2349.3, 0.0625)]):
    x += tone(hz, i) * lfo(rate, 0.0, 1.0, i * 2.1) ** 3
x += 0.25 * band(noise(), 4000, 10000) * lfo(0.0625, 0.1, 1.0)
save('7-shimmer-whisper.wav', x, 0.6)

# 8. Halo: a barely-there low hum under very slow, pure overtones.
x = 0.25 * tone(73.4) + 0.12 * tone(110.0, 0.5)
for i, (hz, rate) in enumerate([(880, 0.0625), (1318.5, 0.0625), (1760, 0.125)]):
    x += 0.35 * tone(hz, i) * lfo(rate, 0.0, 1.0, i * 2.4) ** 2
save('8-shimmer-halo.wav', x, 0.5)

# 9. Breath: the overtones swell and fade together, like slow breathing, over faint air.
breath = lfo(0.125, 0.0, 1.0) ** 2
x = np.zeros(N)
for i, hz in enumerate([987.8, 1318.5, 1975.5]):
    x += tone(hz, i * 1.3) / (1 + i)
x *= breath
x += 0.35 * band(noise(), 800, 4000) * (0.3 + 0.7 * breath)
save('9-shimmer-breath.wav', x, 0.6)

# 10. Dust: sparse tiny glints over a whisper of air, like light catching on motes.
x = 0.8 * grains(26, 1800, 4200, 0.35)
x += 0.3 * band(noise(), 2500, 8000) * lfo(0.0625, 0.3, 1.0)
save('10-shimmer-dust.wav', x, 0.7)

# 11. Low glow: the shimmer an octave down, warmer and softer, slowly turning.
x = 0.3 * tone(55.0)
for i, (hz, rate) in enumerate([(440, 0.0625), (659.3, 0.125), (880, 0.1875)]):
    x += 0.3 * tone(hz, i) * lfo(rate, 0.1, 1.0, i * 1.9) ** 2
x += 0.12 * band(noise(), 300, 1500) * lfo(0.0625, 0.3, 1.0)
save('11-shimmer-low-glow.wav', x, 0.5)
