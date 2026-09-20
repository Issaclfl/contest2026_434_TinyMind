#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""24k -> 16k 两版重采样对拍：旧版（丢采样 + 取平均）与新版（抗混叠低通 + 插值）。

离线片段是云 TTS 出的 24 kHz WAV，板上只认 16 kHz，中间那一步转换是听感差异的
来源。这个脚本把两版算法放在一起量：喂已知频率的单音，看输出谱上多出什么。

用法:
    python3 resample_ab_test.py            # 只做单音测试
    python3 resample_ab_test.py clips/     # additionally 用真实片段跑一遍
"""

import math
import os
import struct
import sys

import numpy as np

RATE_IN = 24000
RATE_OUT = 16000
AMP = 6000.0


def old_resample(samples):
    """被替换掉的那版：idx=(k//2)*3，偶样本直取、奇样本取两邻居均值。"""
    frames = len(samples)
    out_frames = (frames * 2) // 3
    out = []
    for k in range(out_frames):
        idx = (k // 2) * 3
        if k % 2 == 0:
            out.append(samples[idx])
        else:
            a = samples[idx + 1]
            b = samples[idx + 2] if idx + 2 < frames else a
            out.append((a + b) // 2)
    return np.array(out, dtype=np.float64)


def new_resample(samples):
    """现在这版：63 抽头 Blackman 窗 sinc 低通（截止 7 kHz）后再线性插值。

    低通必须在降采样之前做：24 kHz 里 8-12 kHz 的分量（中文的 s/sh/x/c/q 就在
    这一段）不滤掉，会直接折回 0-4 kHz，听感是发沙、发毛。
    """
    cutoff = 7000.0 / RATE_IN
    taps_n = 63
    half = (taps_n - 1) / 2.0
    taps = []
    for i in range(taps_n):
        x = i - half
        h = 2 * cutoff if abs(x) < 1e-9 else math.sin(2 * math.pi * cutoff * x) / (math.pi * x)
        w = (0.42 - 0.5 * math.cos(2 * math.pi * i / (taps_n - 1))
             + 0.08 * math.cos(4 * math.pi * i / (taps_n - 1)))
        taps.append(h * w)
    gain = sum(taps)
    taps = [t / gain for t in taps]

    frames = len(samples)
    src = np.asarray(samples, dtype=np.float64)
    filtered = np.zeros(frames, dtype=np.float64)
    lo = int(half)
    for j, c in enumerate(taps):
        # 卷积按 taps 下标对齐到同一时刻，故滤波后无群延迟，可与原片段逐样本比。
        src_idx = np.arange(frames) + lo - j
        valid = (src_idx >= 0) & (src_idx < frames)
        filtered[valid] += src[src_idx[valid]] * c

    out_frames = (frames * 2) // 3
    pos = np.arange(out_frames) * 1.5
    i0 = np.floor(pos).astype(int)
    frac = pos - i0
    a = filtered[i0]
    b = np.where(i0 + 1 < frames, filtered[np.minimum(i0 + 1, frames - 1)], a)
    return np.clip(a + (b - a) * frac, -32768, 32767)


def spectrum(sig, rate, trim=0.1):
    n = len(sig)
    a = int(n * trim)
    x = sig[a:n - a]
    win = np.hanning(len(x))
    spec = np.abs(np.fft.rfft(x * win)) * 2.0 / win.sum()
    return np.fft.rfftfreq(len(x), 1.0 / rate), spec


def level_at(freqs, spec, f):
    i = int(np.argmin(np.abs(freqs - f)))
    return float(spec[max(0, i - 4):i + 5].max())


def spurious(sig, rate, above):
    freqs, spec = spectrum(sig, rate)
    m = freqs >= above
    if not m.any():
        return 0.0, 0.0
    i = int(np.argmax(np.where(m, spec, 0)))
    return float(spec[i]), float(freqs[i])


def upsample_to_24k(x):
    """16 kHz -> 24 kHz：FFT 理想插值，用来把仓库里的片段还原成"云 TTS 原始波形"。"""
    n_out = int(len(x) * RATE_IN / RATE_OUT)
    spec = np.fft.rfft(x)
    return np.fft.irfft(spec, n=n_out) * (n_out / len(x))


def snr(ref, sig, trim=200):
    n = min(len(ref), len(sig))
    a, b = trim, n - trim
    e = float(np.sum(ref[a:b] ** 2))
    d = float(np.sum((ref[a:b] - sig[a:b]) ** 2))
    return 10 * math.log10(max(e, 1e-9) / max(d, 1e-9))


def tone_test():
    t = np.arange(RATE_IN) / RATE_IN
    print("== 单音频响：喂 24 kHz 正弦，量 16 kHz 输出里该在/不该在的地方（dB，0 = 输入电平）==")
    print("  输入频率     旧版       新版       旧版最大杂散")
    for f in (500, 1000, 2000, 3000, 4000, 5000, 6000, 6500, 7000, 8000, 9000, 10000, 11000):
        x = AMP * np.sin(2 * math.pi * f * t)
        old, new = old_resample(x), new_resample(x)
        d = lambda v: 20 * math.log10(max(v, 1e-9) / AMP)
        lv_old = d(level_at(*spectrum(old, RATE_OUT), f)) if f < 7600 else None
        lv_new = d(level_at(*spectrum(new, RATE_OUT), f)) if f < 7600 else None
        sp, sp_f = spurious(old, RATE_OUT, f + 400 if f < 7600 else 200)
        print("  %6d Hz  %8s  %8s   %7.0f @ %6.0f Hz"
              % (f,
                 ("%7.1f" % lv_old) if lv_old is not None else "    --",
                 ("%7.1f" % lv_new) if lv_new is not None else "    --",
                 sp, sp_f))

    print("\n== 混叠落点：9 kHz / 11 kHz 超出 16 kHz 的奈奎斯特，必然折返 ==")
    for f_in, alias in ((9000, 1000), (11000, 3000)):
        x = AMP * np.sin(2 * math.pi * f_in * t)
        for label, fn in (("旧版", old_resample), ("新版", new_resample)):
            freqs, spec = spectrum(fn(x), RATE_OUT)
            a = level_at(freqs, spec, alias)
            print("  %d Hz 输入 → %s：落点 %d Hz 处 %.1f（%.1f dBc）"
                  % (f_in, label, alias, a, 20 * math.log10(max(a, 1e-9) / AMP)))


def clips_test(clip_dir):
    names = sorted(f[:-4] for f in os.listdir(clip_dir) if f.endswith(".pcm"))
    if not names:
        print("没有找到 .pcm：%s" % clip_dir)
        return
    print("\n== 真实片段：把仓库里的 16 kHz 片段插值回 24 kHz，再分别降回来比 SNR ==")
    print("  片段           时长     旧版 SNR    新版 SNR")
    so = sn = 0.0
    for name in names:
        raw = open(os.path.join(clip_dir, name + ".pcm"), "rb").read()
        x16 = np.array(struct.unpack("<%dh" % (len(raw) // 2), raw), dtype=np.float64)
        x24 = upsample_to_24k(x16)
        a, b = snr(x16, old_resample(x24)), snr(x16, new_resample(x24))
        so += a
        sn += b
        print("  %-12s %5.2f s  %7.1f dB  %7.1f dB" % (name, len(x16) / RATE_OUT, a, b))
    print("  平均：旧版 %.1f dB，新版 %.1f dB" % (so / len(names), sn / len(names)))
    print("\n注：这一项两版接近是正常的——仓库里的片段本身已经被限到 8 kHz 以内，"
          "\n    这段测试量不出旧版丢掉的 8-12 kHz。真实差距看上面的单音测试。")


if __name__ == "__main__":
    tone_test()
    if len(sys.argv) > 1:
        clips_test(sys.argv[1])
