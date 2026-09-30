#!/usr/bin/env python3
"""Measures the reverb in a recording of the reverb kit (d110render --reverb-kit, reverb-kit/ in the source tree).

A real D-110 or D-10/D-20 plays reverb-test-d110.mid or reverb-test-d20.mid; d110emu renders the same file with
    d110render reverb-kit/reverb-test-d110.mid emu.wav --tail 3 [--reverb-settings d110emu-reverb.ini]
This script finds the bursts, subtracts the dry burst from every segment, and measures what is left, the reverb:
    wet_db     its energy against the dry burst's
    pre_ms     when it starts after the burst (first arrival within 20 dB of its peak)
    rt60       its decay at mid frequencies: T20 (-5 to -25 dB of the noise-compensated Schroeder curve) in the
               500 Hz and 1 kHz octave bands, averaged; also broadband and per octave band 250 Hz-8 kHz
    edt        early decay time (0 to -10 dB, times 6), broadband, from the reverb's first arrival
    corr       left/right correlation of the tail
    echoes     the first peaks of its envelope (delays) per channel, ms after the burst

    reverb_analysis.py recording.wav [--schedule FILE] [--compare emu.wav] [--csv out.csv]
                       [--suggest out.ini --settings current.ini]

--compare prints the second file's values beside the first's. --suggest (with --compare, a render made with the
settings in --settings) writes d110emu-reverb.ini values fitted to the first file: RT60 per type and Reverb Time,
pre-delay, wet level per type and Reverb Level, damping, and for the delays their taps and feedback.
Needs numpy.
"""

import argparse
import csv
import math
import os
import struct
import sys

import numpy as np

KIND_NAMES = ["Small Room", "Medium Room", "Medium Hall", "Large Hall", "Plate", "Delay 1", "Delay 2", "Delay 3", "Off"]
BANDS = [250, 500, 1000, 2000, 4000, 8000]
DRY_WINDOW = 0.080   # Seconds of the dry burst subtracted (the burst and the unit's output filters' ringing)
PRE_ROLL = 0.010     # Seconds before the burst kept in each segment
# The burst's end (about 20 ms after its start) moves by a fraction of a millisecond from note to note on a real unit
# (its envelope's clock), so the subtraction leaves a little there: level, decay and arrivals are measured around it.
BURST_END = (0.015, 0.025)
# The model's output taps (mt32emu/src/DSeriesReverb.cpp), for the pre-delay it adds.
TAP_L = [1.0, 0.71, 0.53, 0.87]
TAP_R = [0.61, 1.0, 0.83, 0.47]


# --- Files ---------------------------------------------------------------------------------------------------------

def read_wav(path):
    """Returns (samples[n, 2] as float64 in -1..1, sample rate). Handles PCM 16/24/32-bit, float 32/64, extensible."""
    with open(path, "rb") as f:
        data = f.read()
    if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a WAV file")
    pos = 12
    fmt = None
    payload = None
    while pos + 8 <= len(data):
        chunk, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if chunk == b"fmt ":
            tag, channels, rate = struct.unpack("<HHI", body[0:8])
            bits = struct.unpack("<H", body[14:16])[0]
            if tag == 0xFFFE and len(body) >= 26:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, channels, rate, bits)
        elif chunk == b"data":
            payload = body
        pos += 8 + size + (size & 1)
    if fmt is None or payload is None:
        raise ValueError(f"{path}: no fmt or data chunk")
    tag, channels, rate, bits = fmt
    width = bits // 8
    frames = len(payload) // (width * channels)
    raw = payload[:frames * width * channels]
    if tag == 3:
        samples = np.frombuffer(raw, dtype="<f4" if bits == 32 else "<f8").astype(np.float64)
    elif tag == 1 and bits == 16:
        samples = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif tag == 1 and bits == 24:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        samples = v.astype(np.float64) / float(1 << 23)
    elif tag == 1 and bits == 32:
        samples = np.frombuffer(raw, dtype="<i4").astype(np.float64) / float(1 << 31)
    else:
        raise ValueError(f"{path}: unsupported WAV format {tag}, {bits} bits")
    samples = samples.reshape(-1, channels)
    if channels == 1:
        samples = np.repeat(samples, 2, axis=1)
    return samples[:, :2], rate


def read_schedule(path):
    with open(path, newline="") as f:
        return [{"start": float(r["start"]), "kind": r["kind"], "type": int(r["type"]), "time": int(r["time"]),
                 "level": int(r["level"])} for r in csv.DictReader(f)]


def read_settings(path):
    values = {}
    if path and os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                if "=" in line and not line.lstrip().startswith("#"):
                    key, value = line.split("=", 1)
                    values[key.strip()] = value.strip()
    return values


# --- Alignment -----------------------------------------------------------------------------------------------------

def envelope_db(x, rate, hop=0.001):
    """Energy per `hop` seconds (both channels), in dB."""
    n = int(rate * hop)
    frames = len(x) // n
    e = (x[:frames * n] ** 2).sum(axis=1).reshape(frames, n).sum(axis=1)
    return 10.0 * np.log10(e + 1e-20), n


def find_onsets(x, rate, schedule):
    """Sample positions of every scheduled event, from the dry bursts, refined per event. The recorder's clock and the
    MIDI player's differ (a recording started by hand, a player running 0.1% fast): each event is looked for where the
    ones found so far put it."""
    env, hop = envelope_db(x, rate)
    floor = np.percentile(env[: max(10, int(0.5 / 0.001))], 50) if len(env) > 500 else env.min()
    peak = env.max()
    threshold = max(floor + 25.0, peak - 45.0)
    first = int(np.argmax(env > threshold))
    # Every event near where the schedule puts it: the first rise above the threshold.
    onsets = []
    found = [(schedule[0]["start"], first * hop)]
    for event in schedule:
        if len(found) >= 2 and found[-1][0] > found[0][0]:
            rate_ratio = (found[-1][1] - found[0][1]) / (found[-1][0] - found[0][0])
        else:
            rate_ratio = rate
        expected = found[-1][1] + (event["start"] - found[-1][0]) * rate_ratio
        lo, hi = int((expected - 0.15 * rate) / hop), int((expected + 0.15 * rate) / hop)
        lo, hi = max(lo, 0), min(hi, len(env))
        above = np.nonzero(env[lo:hi] > threshold)[0]
        if len(above):
            onsets.append((lo + above[0]) * hop)
            found.append((event["start"], onsets[-1]))
        else:
            onsets.append(None)
    # A straight line through them corrects clock drift; events not found take it.
    pairs = [(e["start"], o) for e, o in zip(schedule, onsets) if o is not None]
    t = np.array([p[0] for p in pairs])
    s = np.array([p[1] for p in pairs], dtype=np.float64)
    slope, intercept = np.polyfit(t, s, 1)
    return [int(round(o)) if o is not None else int(round(slope * e["start"] + intercept)) for e, o in zip(schedule, onsets)], slope / rate


def refine(x, position, reference, rate):
    """Moves `position` to where the dry burst `reference` fits best (cross-correlation over +-3 ms)."""
    span = int(0.003 * rate)
    n = len(reference)
    lo = max(position - span - int(PRE_ROLL * rate), 0)
    window = x[lo:lo + n + 2 * span, :].sum(axis=1)
    ref = reference.sum(axis=1)
    if len(window) < n + 2 * span:
        return position
    best, best_lag = -1e30, 0
    for lag in range(2 * span + 1):
        c = float(np.dot(window[lag:lag + n], ref))
        if c > best:
            best, best_lag = c, lag
    return lo + best_lag + int(PRE_ROLL * rate)


def fractional_fit(segment, reference, window):
    """The reference moved by the fraction of a sample (and scaled per channel) that fits the segment's first
    `window` samples best. A unit's DAC runs on its own clock (the D-20's at 32 kHz), so its bursts land between the
    recorder's samples differently each time: moved to the nearest sample, a noise burst leaves about -10 dB behind,
    moved exactly, about -40 dB."""
    n = len(reference)
    size = 4 * n
    a = np.fft.rfft(segment[:window], n=size, axis=0)
    b = np.fft.rfft(reference[:window], n=size, axis=0)
    up = 32
    cc = np.fft.irfft((a * np.conj(b)).sum(axis=1), n=size * up)
    k = int(np.argmax(cc))
    shift = (k if k < size * up // 2 else k - size * up) / up
    freqs = np.fft.rfftfreq(size)
    moved = np.fft.irfft(np.fft.rfft(reference, n=size, axis=0) * np.exp(-2j * np.pi * freqs * shift)[:, None], n=size, axis=0)[:n]
    gains = []
    for ch in range(2):
        ref = moved[:window, ch]
        gains.append(float(np.dot(segment[:window, ch], ref) / (np.dot(ref, ref) or 1e-30)))
    return moved * np.array(gains)[None, :], shift, gains


# --- Measurements --------------------------------------------------------------------------------------------------

def band_filter(spectrum, n, rate, center):
    """Octave band around `center`, per channel, of a signal of `n` samples whose spectrum (rfft with n=2*n, so that
    nothing wraps around) is given: a mask with raised-cosine edges a third of an octave wide."""
    freqs = np.fft.rfftfreq(2 * n, 1.0 / rate)
    octaves = np.log2(np.maximum(freqs, 1.0) / center)
    edge = 1.0 / 6.0  # Half the transition, in octaves
    mask = np.clip((0.5 + edge - np.abs(octaves)) / (2 * edge), 0.0, 1.0)
    mask = 0.5 - 0.5 * np.cos(math.pi * mask)
    return np.fft.irfft(spectrum * mask[:, None], n=2 * n, axis=0)[:n]


def decay(wet, rate, noise_power):
    """(T20-based RT60, EDT) from the Schroeder curve of `wet` (n x 2), noise compensated; None where unreliable."""
    power = (wet ** 2).sum(axis=1)
    # Stop where the smoothed envelope reaches the noise floor.
    hop = max(1, int(0.005 * rate))
    frames = len(power) // hop
    env = power[:frames * hop].reshape(frames, hop).mean(axis=1)
    above = np.nonzero(env > noise_power * 3.0)[0]
    if len(above) < 4:
        return None, None
    end = (above[-1] + 1) * hop
    p = np.maximum(power[:end] - noise_power, 0.0)
    edc = np.cumsum(p[::-1])[::-1]
    if edc[0] <= 0:
        return None, None
    edc_db = 10.0 * np.log10(edc / edc[0] + 1e-30)
    t = np.arange(len(edc_db)) / rate

    def fit(top, bottom):
        idx = np.nonzero((edc_db <= top) & (edc_db >= bottom))[0]
        if len(idx) < rate * 0.01:
            return None
        slope = np.polyfit(t[idx], edc_db[idx], 1)[0]
        return -60.0 / slope if slope < 0 else None

    return fit(-5.0, -25.0), (None if fit(0.0, -10.0) is None else fit(0.0, -10.0))


def echoes(wet, rate, max_seconds=1.6, spacing=0.025):
    """Each channel's echoes: when they start (ms, first 1-ms step within 12 dB of their peak) and their energy (dB
    against the loudest), for echoes at least `spacing` seconds apart (closer ones merge with the burst's length)."""
    result = []
    hop = max(1, int(0.001 * rate))
    gap = int(spacing * 1000.0)
    for ch in range(2):
        p = wet[: int(max_seconds * rate), ch] ** 2
        frames = len(p) // hop
        env = p[:frames * hop].reshape(frames, hop).sum(axis=1)
        top = env.max() if len(env) else 0.0
        found = []
        i = 0
        while i < len(env) and top > 0:
            if env[i] > top * 10 ** (-24 / 10):
                window = env[i:i + gap]
                energy = float(window.sum())
                peak = float(window.max())
                start = i + int(np.argmax(window > peak * 10 ** (-12 / 10)))
                found.append((start * hop * 1000.0 / rate, energy))
                i += gap
            else:
                i += 1
        if found:
            loudest = max(e for _, e in found)
            result.append([(t, 10 * math.log10(e / loudest)) for t, e in found[:6]])
        else:
            result.append([])
    return result


def analyse(path, schedule):
    x, rate = read_wav(path)
    onsets, drift = find_onsets(x, rate, schedule)
    dry_events = [i for i, e in enumerate(schedule) if e["kind"] == "dry"]
    n_dry = int(DRY_WINDOW * rate)
    pre = int(PRE_ROLL * rate)
    # The dry burst: the dry events, aligned on the first one to the fraction of a sample, averaged.
    first = onsets[dry_events[0]]
    reference = x[first - pre:first - pre + n_dry + pre].copy()
    burst_window = pre + int(0.012 * rate)
    aligned = []
    for i in dry_events:
        pos = refine(x, onsets[i], reference, rate)
        moved, _, _ = fractional_fit(reference, x[pos - pre:pos - pre + n_dry + pre], burst_window)
        aligned.append(moved)
    dry = np.mean(aligned, axis=0)
    dry_energy = float((dry ** 2).sum())
    # The noise floor: the quietest 100 ms before the first dry burst.
    lead = x[: max(first - int(0.05 * rate), int(0.2 * rate))]
    chunks = [lead[i:i + int(0.1 * rate)] for i in range(0, len(lead) - int(0.1 * rate), int(0.05 * rate))]
    noise_power = min(float((c ** 2).sum(axis=1).mean()) for c in chunks) if chunks else 1e-12
    results = []
    for index, event in enumerate(schedule):
        if event["kind"] != "burst":
            continue
        pos = refine(x, onsets[index], dry, rate)
        following = [s for s in schedule if s["start"] > event["start"]]
        length = int(((following[0]["start"] - event["start"]) if following else 5.0) * rate) - int(1.2 * rate)
        segment = x[pos - pre:pos - pre + length].copy()
        # The dry burst, moved to the fraction of a sample and scaled per channel to this segment's burst (a drifting
        # balance or volume), fitted on its first 12 ms, then taken away.
        n = min(len(dry), len(segment))
        scaled, _, gains = fractional_fit(segment[:n], dry[:n], burst_window)
        segment[:n] -= scaled
        wet = segment[pre:]
        dry_here = float((scaled ** 2).sum())
        masked = wet.copy()
        masked[int(BURST_END[0] * rate):int(BURST_END[1] * rate)] = 0.0
        wet_energy = max(float((masked ** 2).sum()) - noise_power * (len(wet) - int((BURST_END[1] - BURST_END[0]) * rate)), 0.0)
        power = (masked ** 2).sum(axis=1)
        hop = max(1, int(0.001 * rate))
        frames = len(power) // hop
        env = power[:frames * hop].reshape(frames, hop).sum(axis=1)
        peak = env.max() if frames else 0.0
        # What the subtraction leaves in the burst's first milliseconds (none in a render) is no arrival.
        residue = float(np.median(env[:int(BURST_END[0] * 1000) - 3])) if frames > 20 else 0.0
        arrivals = np.nonzero(env > max(peak * 0.01, residue * 10.0))[0]
        arrival = arrivals[0] * hop if len(arrivals) else 0
        tail = wet[max(arrival, int(BURST_END[1] * rate)):]
        broad, edt = decay(tail, rate, noise_power)
        bands = {}
        spectrum = np.fft.rfft(tail, n=2 * len(tail), axis=0)
        for center in BANDS:
            if center * math.sqrt(2) < rate / 2:
                bands[center] = decay(band_filter(spectrum, len(tail), rate, center), rate, noise_power / 6.0)[0]
        mids = [bands.get(500), bands.get(1000)]
        rt60 = sum(mids) / 2.0 if all(mids) else broad
        first_second = wet[: int(1.0 * rate)]
        # The reverb's tilt against the dry burst's: its 4 kHz and 8 kHz octaves against its 1 kHz one (dB).
        wet_spectrum = (np.abs(np.fft.rfft(masked[: int(1.0 * rate)], axis=0)) ** 2).sum(axis=1)
        dry_spectrum = (np.abs(np.fft.rfft(scaled, n=int(1.0 * rate), axis=0)) ** 2).sum(axis=1)
        freqs = np.fft.rfftfreq(int(1.0 * rate), 1.0 / rate)

        def octave(spectrum, center):
            return float(spectrum[(freqs >= center / math.sqrt(2)) & (freqs < center * math.sqrt(2))].sum()) + 1e-30

        tilt = {c: 10 * math.log10((octave(wet_spectrum, c) / octave(wet_spectrum, 1000)) / (octave(dry_spectrum, c) / octave(dry_spectrum, 1000)))
                for c in (4000, 8000) if c * math.sqrt(2) < rate / 2}
        denom = math.sqrt(float((first_second[:, 0] ** 2).sum() * (first_second[:, 1] ** 2).sum())) or 1e-30
        results.append({
            "type": event["type"], "time": event["time"], "level": event["level"],
            "wet_db": 10 * math.log10(wet_energy / dry_here) if wet_energy > 0 and dry_here > 0 else -120.0,
            "gains": gains,
            "tilt": tilt,
            "pre_ms": arrivals[0] * hop * 1000.0 / rate if len(arrivals) else None,
            "rt60": rt60, "rt60_broad": broad, "edt": edt, "bands": bands,
            "corr": float((first_second[:, 0] * first_second[:, 1]).sum()) / denom,
            "echoes": echoes(masked, rate),
        })
    return results, {"rate": rate, "drift_ppm": (drift - 1.0) * 1e6, "noise_db": 10 * math.log10(noise_power + 1e-30),
                     "dry_db": 10 * math.log10(dry_energy / n_dry + 1e-30)}


# --- Output --------------------------------------------------------------------------------------------------------

def fmt(value, spec):
    return format(value, spec) if value is not None else "-"


def print_results(results, info, label, other=None):
    print(f"{label}: {info['rate']} Hz, clock {info['drift_ppm']:+.0f} ppm, dry burst {info['dry_db']:.1f} dB, "
          f"noise floor {info['noise_db']:.1f} dB per sample")
    header = f"{'type':<12}{'time':>5}{'lvl':>4}{'wet dB':>8}{'pre ms':>8}{'RT60':>7}{'broad':>7}{'EDT':>7}{'corr':>6}  bands RT60 (250-8k)"
    print(header + ("   |  other: wet dB, pre, RT60" if other else ""))
    for i, r in enumerate(results):
        bands = " ".join(fmt(r["bands"].get(c), ".2f") for c in BANDS if c in r["bands"])
        line = (f"{KIND_NAMES[r['type']]:<12}{r['time']:>5}{r['level']:>4}{r['wet_db']:>8.1f}{fmt(r['pre_ms'], '8.1f')}"
                f"{fmt(r['rt60'], '7.2f')}{fmt(r['rt60_broad'], '7.2f')}{fmt(r['edt'], '7.2f')}{r['corr']:>6.2f}  ")
        if r["type"] >= 5:
            line += ("echoes L " + ",".join(f"{t:.0f}({d:.0f})" for t, d in r["echoes"][0][:4]) + "  R "
                     + ",".join(f"{t:.0f}({d:.0f})" for t, d in r["echoes"][1][:4]))
        else:
            line += bands
        if other:
            o = other[i]
            line += f"   |  {o['wet_db']:6.1f} {fmt(o['pre_ms'], '6.1f')} {fmt(o['rt60'], '5.2f')}"
        print(line)


def write_csv(path, results):
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["type", "name", "time", "level", "wet_db", "pre_ms", "rt60", "rt60_broad", "edt", "corr"] + [f"rt60_{c}" for c in BANDS]
                   + ["echoes_l_ms", "echoes_r_ms"])
        for r in results:
            w.writerow([r["type"] + 1, KIND_NAMES[r["type"]], r["time"], r["level"], f"{r['wet_db']:.2f}", fmt(r["pre_ms"], ".2f"),
                        fmt(r["rt60"], ".3f"), fmt(r["rt60_broad"], ".3f"), fmt(r["edt"], ".3f"), f"{r['corr']:.3f}"]
                       + [fmt(r["bands"].get(c), ".3f") for c in BANDS]
                       + [" ".join(f"{t:.1f}" for t, _ in r["echoes"][0]), " ".join(f"{t:.1f}" for t, _ in r["echoes"][1])])


# --- Fitting -------------------------------------------------------------------------------------------------------

def parse_list(text, default):
    try:
        values = [float(v) for v in text.split(",")]
        return values if len(values) == len(default) else default
    except (AttributeError, ValueError):
        return default


def lowpass_tilt(cutoff, high, rate=32000.0, low=1000.0):
    """dB between `high` and `low` through the model's input low-pass (2nd order, Q 0.5, bilinear) at `cutoff`."""
    w0 = 2 * math.pi * min(cutoff, 0.45 * rate) / rate
    alpha = math.sin(w0) / (2 * 0.5)
    cosw = math.cos(w0)
    a0 = 1 + alpha
    b = [(1 - cosw) / 2 / a0, (1 - cosw) / a0, (1 - cosw) / 2 / a0]
    a = [1.0, -2 * cosw / a0, (1 - alpha) / a0]
    def gain(f):
        z = complex(math.cos(2 * math.pi * f / rate), -math.sin(2 * math.pi * f / rate))
        return abs((b[0] + b[1] * z + b[2] * z * z) / (a[0] + a[1] * z + a[2] * z * z))
    return 20 * math.log10(gain(high) / gain(low))


def suggest(path, measured, emulated, current):
    """Writes d110emu-reverb.ini values moving the model (which rendered `emulated` with `current`) to `measured`."""
    lines = ["# Fitted by tools/reverb_analysis.py: load in d110emu's Reverb tuning window, or pass to d110render"]
    by_key = lambda rs: {(r["type"], r["time"], r["level"]): r for r in rs}
    m, e = by_key(measured), by_key(emulated)
    median = lambda values: float(np.median(values)) if values else None
    # The Reverb Level curve (dB against level 7), from the unit's Medium Hall at Reverb Time 4, less what its level 0
    # holds (noise, what the subtraction leaves): level 0 is silent on the units.
    sweep = {r["level"]: r for r in measured if r["type"] == 2 and r["time"] == 4}
    floor = 10 ** (sweep[0]["wet_db"] / 10) if 0 in sweep else 0.0

    def cleaned(db):
        power = 10 ** (db / 10) - floor
        return 10 * math.log10(power) if power > 0 else None

    curve = {}
    if 7 in sweep and cleaned(sweep[7]["wet_db"]) is not None:
        top = cleaned(sweep[7]["wet_db"])
        for level in range(1, 7):
            value = cleaned(sweep[level]["wet_db"]) if level in sweep else None
            if value is not None and value - top > -40:
                curve[level] = value - top
    for t in range(8):
        p = f"type{t + 1}."
        lines.append(f"{p}name = {KIND_NAMES[t]}")
        wet = parse_list(current.get(p + "wet_db"), [-60, -27, -24, -21, -18, -15, -12, -9])
        diffs = [m[(t, time, 7)]["wet_db"] - e[(t, time, 7)]["wet_db"] for time in range(1, 9)
                 if (t, time, 7) in m and (t, time, 7) in e and m[(t, time, 7)]["wet_db"] > -60]
        level7 = wet[7] + (sum(diffs) / len(diffs) if diffs else 0.0)
        new_wet = [wet[0]] + [level7 + curve[level] if level in curve else level7 + wet[level] - wet[7] for level in range(1, 7)] + [level7]
        lines.append(p + "wet_db = " + ",".join(f"{v:.1f}" for v in new_wet))
        # The input low-pass, from the tilt of the reverb against the dry burst at 4 and 8 kHz (with the damping).
        bandwidth = float(current.get(p + "bandwidth_hz", "8000") or 8000)
        targets = {}
        for band in (4000, 8000):
            tilts = [m[(t, time, 7)]["tilt"].get(band) - e[(t, time, 7)]["tilt"].get(band) for time in range(1, 9)
                     if (t, time, 7) in m and (t, time, 7) in e and m[(t, time, 7)]["tilt"].get(band) is not None
                     and e[(t, time, 7)]["tilt"].get(band) is not None]
            if tilts:
                targets[band] = lowpass_tilt(bandwidth, band) + median(tilts)
        if targets:
            candidates = [200.0 * 1.02 ** k for k in range(220)]
            bandwidth = min(candidates, key=lambda f: sum((lowpass_tilt(f, band) - target) ** 2 for band, target in targets.items()))
        lines.append(f"{p}bandwidth_hz = {min(max(bandwidth, 200.0), 16000.0):.0f}")
        if t < 5:
            rt60 = parse_list(current.get(p + "rt60"), [1.0] * 8)
            new_rt60 = []
            for time in range(1, 9):
                mm, ee = m.get((t, time, 7)), e.get((t, time, 7))
                if mm and ee and mm["rt60"] and ee["rt60"]:
                    new_rt60.append(rt60[time - 1] * mm["rt60"] / ee["rt60"])
                else:
                    new_rt60.append(rt60[time - 1])
            lines.append(p + "rt60 = " + ",".join(f"{v:.3f}" for v in new_rt60))
            unit = [m[(t, time, 7)]["pre_ms"] for time in range(1, 9) if (t, time, 7) in m and m[(t, time, 7)]["pre_ms"] is not None]
            model = [e[(t, time, 7)]["pre_ms"] for time in range(1, 9) if (t, time, 7) in e and e[(t, time, 7)]["pre_ms"] is not None]
            pre = float(current.get(p + "pre_delay_ms", "0") or 0)
            if unit and model:
                pre += median(unit) - median(model)
            lines.append(f"{p}pre_delay_ms = {max(pre, 0.0):.1f}")
            # Damping: the cutoff moved by how much sooner (or later) the unit's 4 kHz band dies than the model's.
            ratios = []
            for time in range(3, 9):
                mm, ee = m.get((t, time, 7)), e.get((t, time, 7))
                if mm and ee and all(r["bands"].get(c) for r in (mm, ee) for c in (500, 4000)):
                    ratios.append((mm["bands"][4000] / mm["bands"][500]) / (ee["bands"][4000] / ee["bands"][500]))
            damping = float(current.get(p + "damping_hz", "4000") or 4000)
            if ratios:
                damping *= median(ratios) ** 2.0
            lines.append(f"{p}damping_hz = {min(max(damping, 200.0), 16000.0):.0f}")
        else:
            left, right, feedback, single = [], [], [], 0
            old_feedback = float(current.get(p + "feedback", "0.3") or 0.3)
            for time in range(1, 9):
                mm, ee = m.get((t, time, 7)), e.get((t, time, 7))
                strong = lambda echoes: [echo for echo in echoes if echo[1] > -10.0]
                if mm and strong(mm["echoes"][0]) and strong(mm["echoes"][1]):
                    # The first strong echo of each channel (the unit's taps; fainter ones are what the subtraction
                    # leaves or noise).
                    left.append(strong(mm["echoes"][0])[0][0])
                    right.append(strong(mm["echoes"][1])[0][0])
                    mr, er = mm["echoes"][1], (ee["echoes"][1] if ee else [])
                    if len(mr) < 2:
                        single += 1
                    elif len(er) > 1:
                        # The second echo against the first, the unit's and the model's (both lose the same to the
                        # damping): the ratio moves the feedback.
                        feedback.append(max(old_feedback, 0.05) * 10 ** (((mr[1][1] - mr[0][1]) - (er[1][1] - er[0][1])) / 20.0))
                else:
                    left.append(None)
                    right.append(None)
            old_l = parse_list(current.get(p + "delay_l_ms"), [100.0] * 8)
            old_r = parse_list(current.get(p + "delay_r_ms"), [100.0] * 8)
            lines.append(p + "delay_l_ms = " + ",".join(f"{v if v is not None else o:.1f}" for v, o in zip(left, old_l)))
            lines.append(p + "delay_r_ms = " + ",".join(f"{v if v is not None else o:.1f}" for v, o in zip(right, old_r)))
            if single > 4:
                lines.append(f"{p}feedback = 0")  # A single echo: nothing goes round
            elif feedback:
                lines.append(f"{p}feedback = {min(float(np.median(feedback)), 0.95):.3f}")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("recording")
    here = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument("--schedule", default=os.path.join(here, "..", "reverb-kit", "reverb-test-schedule.csv"))
    parser.add_argument("--compare")
    parser.add_argument("--csv")
    parser.add_argument("--suggest")
    parser.add_argument("--settings", help="the d110emu-reverb.ini the --compare render used")
    args = parser.parse_args()
    schedule = read_schedule(args.schedule)
    results, info = analyse(args.recording, schedule)
    other = None
    if args.compare:
        other, other_info = analyse(args.compare, schedule)
    print_results(results, info, os.path.basename(args.recording), other)
    if args.csv:
        write_csv(args.csv, results)
    if args.suggest:
        if other is None:
            sys.exit("--suggest needs --compare (a render of the kit with the current settings)")
        suggest(args.suggest, results, other, read_settings(args.settings))
        print(f"wrote {args.suggest}")


if __name__ == "__main__":
    main()
