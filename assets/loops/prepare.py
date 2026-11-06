#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Makes the bundled loops from the Guitar-TECHS dataset's direct-input takes, and their manifest.json and ATTRIBUTION.md.
# The WAV files are committed, so nothing needs this script to run: it is the record of how they were made.
#   curl -LO https://zenodo.org/records/14963133/files/P3_music.zip && unzip P3_music.zip
#   python3 assets/loops/prepare.py P3_music        (needs numpy and scipy)
# Each loop runs from a note to the same note some bars later, found by comparing the spectra around both. The end point
# is moved to where the two waveforms line up, then the end's first 20 ms are crossfaded into the start.
import hashlib, json, os, sys, wave
import numpy as np
import scipy.io.wavfile
import scipy.signal

HERE = os.path.dirname(os.path.abspath(__file__))
RATE = 44100
PEAK = 10 ** (-3 / 20)
FADE = int(0.020 * RATE)
ALIGN = int(0.008 * RATE)
AUTHOR = "Hegel Emmanuel Pedroza Villalobos, Termeh Taheri, Wallace Abreu, Ryan Corey and Iran R. Roman"
SOURCE = "https://zenodo.org/records/14963133"
LICENSE, LICENSE_URL = "CC-BY-4.0", "https://creativecommons.org/licenses/by/4.0/"

# take, seconds from, to, BPM of the take's MIDI file, bars
LOOPS = [
    ("chug", "Low riff", "chugging rhythm", 2, (14.66, 20.054, 90, 2)),
    ("riff", "Clean riff", "clean riff", 8, (15.57, 19.987, 110, 2)),
    ("arpeggio", "Arpeggios", "arpeggios", 9, (18.078, 23.546, 89, 2)),
    ("chords", "Chords", "chords", 11, (2.377, 7.487, 93, 2)),
    ("lead", "Lead phrase", "lead phrase", 12, (3.275, 13.566, 140, 6)),
]


def take(root, number):
    _, x = scipy.io.wavfile.read(os.path.join(root, "audio", "directinput", f"directinput_{number:02d}.wav"))
    x = x[:, 0] if x.ndim > 1 else x
    return scipy.signal.resample_poly(x / float(2 ** 31), 147, 160)


def loop(x, start, end):
    s, e = round(start * RATE), round(end * RATE)
    window = x[s : s + 2048]
    lags = range(-ALIGN, ALIGN + 1)
    e += max(lags, key=lambda lag: float(np.dot(window, x[e + lag : e + lag + 2048])))
    y = x[s:e].copy()
    fade = 0.5 - 0.5 * np.cos(np.pi * np.arange(FADE) / FADE)
    y[:FADE] = y[:FADE] * fade + x[e : e + FADE] * (1 - fade)
    return y * (PEAK / np.abs(y).max())


def write(path, y):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(np.round(y * 32767).astype("<i2").tobytes())


def main(root):
    entries = []
    for name, title, kind, number, (start, end, bpm, bars) in LOOPS:
        y = loop(take(root, number), start, end)
        file = f"{name}.wav"
        write(os.path.join(HERE, file), y)
        with open(os.path.join(HERE, file), "rb") as f:
            sha = hashlib.sha256(f.read()).hexdigest()
        seam = abs(y[0] - y[-1])
        print(f"{file}: {len(y) / RATE:.3f} s, seam step {seam:.5f}, largest step {np.abs(np.diff(y)).max():.5f}")
        entries.append({
            "file": file, "title": title, "kind": kind, "signal": "dry", "author": AUTHOR, "source": SOURCE,
            "source_file": f"P3_music.zip: audio/directinput/directinput_{number:02d}.wav, {start} s to {end} s",
            "license": LICENSE, "license_url": LICENSE_URL,
            "attribution": f"\"{title}\", cut from Guitar-TECHS (directinput_{number:02d}.wav of P3_music) by {AUTHOR}, {SOURCE}, CC BY 4.0. "
                           "Changed: cut to a loop, crossfaded, resampled to 44.1 kHz, converted to mono 16-bit, normalised.",
            "sha256": sha, "seconds": round(len(y) / RATE, 3), "bpm": bpm, "bars": bars, "sample_rate": RATE, "channels": 1,
        })
    with open(os.path.join(HERE, "manifest.json"), "w") as f:
        json.dump({"loops": entries}, f, indent=2)
        f.write("\n")
    with open(os.path.join(HERE, "ATTRIBUTION.md"), "w") as f:
        f.write("# The bundled guitar loops\n\n"
                "These recordings are not under this repository's GPL-3.0. They are licensed under the "
                f"[Creative Commons Attribution 4.0 International licence]({LICENSE_URL}), as the dataset's Zenodo record states "
                f"([{SOURCE}]({SOURCE}), read on 2026-11-05). All are the dry direct-input channel of the guitar, with no amp.\n\n"
                "The dataset's authors ask users to cite their paper: Guitar-TECHS, ICASSP 2025, "
                "doi:10.1109/ICASSP49660.2025.10887996.\n\n"
                "`prepare.py` records how each file was made. BPM values come from the dataset's MIDI files.\n\n")
        for e in entries:
            f.write(f"- `{e['file']}`: {e['attribution']}\n")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: prepare.py P3_MUSIC_FOLDER")
    main(sys.argv[1])
