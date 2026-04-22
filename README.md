# piper-opus-tts

TTS engine that synthesizes directly to OGG/Opus bytes in C++. No temp files, no subprocesses, no WAV handed off to Python — text in, streamable audio out.

Works well as a drop-in for WebSocket servers, VoIP stacks, and announcement systems where latency actually matters.

---

## Internals

```
text + voice params
        │
        ▼
[ C++  tts_engine.so ]
        │
        ├─ piper::textToAudio()     text → phonemes (espeak-ng) → ONNX inference → int16 PCM
        │
        ├─ resample()               PCM resampled to 48 kHz (linear interpolation, no extra deps)
        │
        ├─ apply_volume_db()        gain applied directly on PCM samples
        │
        └─ encode_opus_ogg()        libopus frame encoding → libogg container
                │
                ▼
        OGG/Opus bytes              returned to Python as bytes — ready to stream
```

One C++ call covers phonemization through Opus encoding. The GIL is released during inference so threads genuinely run in parallel.

---

## Latency

| Approach | Per call | Notes |
|---|---|---|
| Python `piper-tts` + `pydub` + ffmpeg subprocess | ~300–700 ms | subprocess fork + pipe + temp file each time |
| C++ piper + Python subprocess to ffmpeg | ~200–500 ms | no temp file but subprocess overhead stays |
| **This engine** | **~150–400 ms** | single C++ call, no subprocess, no disk I/O |

The bottleneck is ONNX inference and it scales with text length. Everything else — phonemization, resampling, Opus encoding — is under 10 ms combined.

---

## Project structure

```
├── tts_engine.cpython-312-*.so    # built by build.sh, gitignored
│
├── src/
│   └── tts_module.cpp             # piper + resampler + opus encoder, all in one shot
│
├── build/
│   ├── CMakeLists.txt
│   ├── build.sh                   # fetches deps and compiles everything
│   └── out/                       # cmake output, gitignored
│
└── dependencies/                  # runtime libs and voice models
    ├── *.onnx / *.onnx.json       # downloaded separately (see below)
    ├── espeak-ng-data/
    ├── include/
    ├── lib/
    ├── libespeak-ng.so.1
    ├── libonnxruntime.so.1.14.1
    ├── libpiper_phonemize.so.1
    └── piper/                     # cloned by build.sh
```

---

## Supported voices

| Language | Gender | Model file |
|---|---|---|
| Polish | female | `pl_PL-gosia-medium.onnx` |
| Polish | male | `pl_PL-darkman-medium.onnx` |
| English | female | `en_US-amy-medium.onnx` |
| English | male | `en_US-ryan-medium.onnx` |
| German | female | `de_DE-eva_k-x_low.onnx` |
| German | male | `de_DE-thorsten-medium.onnx` |

Models come from [rhasspy/piper-voices on Hugging Face](https://huggingface.co/rhasspy/piper-voices). Each voice needs two files: `.onnx` (weights) and `.onnx.json` (config). Place both in `dependencies/`.

---

## Installation

### Linux (native)

**1. System dependencies**

```bash
sudo apt-get install -y \
    build-essential git python3-dev \
    libopus-dev libogg-dev pkg-config
```

**2. Clone and build**

```bash
git clone https://github.com/DexusY/piper-opus-tts.git
cd piper-opus-tts
bash build/build.sh
```

`build.sh` clones piper, pulls pre-built piper-phonemize (headers + runtime libs), downloads onnxruntime and spdlog headers, then compiles `tts_module.cpp` into `tts_engine.cpython-*.so`.

**3. Download voice models**

```bash
BASE="https://huggingface.co/rhasspy/piper-voices/resolve/main"

wget "$BASE/pl/pl_PL/gosia/medium/pl_PL-gosia-medium.onnx"              -P dependencies/
wget "$BASE/pl/pl_PL/gosia/medium/pl_PL-gosia-medium.onnx.json"         -P dependencies/

wget "$BASE/pl/pl_PL/darkman/medium/pl_PL-darkman-medium.onnx"          -P dependencies/
wget "$BASE/pl/pl_PL/darkman/medium/pl_PL-darkman-medium.onnx.json"     -P dependencies/

wget "$BASE/en/en_US/amy/medium/en_US-amy-medium.onnx"                  -P dependencies/
wget "$BASE/en/en_US/amy/medium/en_US-amy-medium.onnx.json"             -P dependencies/

wget "$BASE/en/en_US/ryan/medium/en_US-ryan-medium.onnx"                -P dependencies/
wget "$BASE/en/en_US/ryan/medium/en_US-ryan-medium.onnx.json"           -P dependencies/

wget "$BASE/de/de_DE/eva_k/x_low/de_DE-eva_k-x_low.onnx"               -P dependencies/
wget "$BASE/de/de_DE/eva_k/x_low/de_DE-eva_k-x_low.onnx.json"          -P dependencies/

wget "$BASE/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx"        -P dependencies/
wget "$BASE/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx.json"   -P dependencies/
```

**4. Run the test**

```bash
python main.py
```

Expected output:

```
[tts] Using C++ backend — piper inference + libopus encoding
  [standard]            12916 bytes → test_standard.opus
  [louder (+5 dB)]      12641 bytes → test_louder_(plus5_dB).opus
  [expressive]          12648 bytes → test_expressive.opus
  [slow]                15858 bytes → test_slow.opus
  [fast + low bitrate]   7590 bytes → test_fast_plus_low_bitrate.opus
```

---

### Windows (via WSL2)

Native Windows isn't supported — piper-phonemize ships as a Linux x86_64 binary and the build tooling is Linux-only. WSL2 runs it at native Linux performance with no overhead.

**1. Install WSL2**

```powershell
wsl --install
```

Restart, then open the Ubuntu terminal.

**2. Follow the Linux instructions inside WSL2**

```bash
sudo apt-get update
sudo apt-get install -y build-essential git python3-dev libopus-dev libogg-dev pkg-config

git clone https://github.com/DexusY/piper-opus-tts.git
cd piper-opus-tts
bash build/build.sh
```

Download voice models and run `python main.py` as above.

Your WSL2 home is accessible from Explorer at `\\wsl$\Ubuntu\home\<username>\`.

---

## Usage

```python
from main import synthesize

# returns OGG/Opus bytes directly
opus: bytes = synthesize("Hello, world!", language="english", gender="female")

await websocket.send(opus)
```

### `synthesize(text, language, gender, volume_db, noise_scale, length_scale, bitrate_kbps)`

| Parameter | Type | Default | Description |
|---|---|---|---|
| `text` | `str` | — | UTF-8 text to speak |
| `language` | `str` | `"polish"` | `"polish"`, `"english"`, `"german"` |
| `gender` | `str` | `"female"` | `"female"`, `"male"` |
| `volume_db` | `float` | `0.0` | Volume shift in dB (`+5` louder, `-5` quieter) |
| `noise_scale` | `float` | `0.667` | Phoneme variation — higher sounds more natural, above `1.0` becomes unstable |
| `length_scale` | `float` | `1.0` | Speaking rate — `0.8` faster, `1.3` slower |
| `bitrate_kbps` | `int` | `24` | Opus bitrate: `16` smallest, `24` default speech quality, `48` transparent |

Returns `bytes` — OGG/Opus audio ready to write to disk or push over any transport.

---

## Example (`main.py`)

```python
import os
import sys
from typing import Literal

Language = Literal["polish", "english", "german"]
Gender   = Literal["male", "female"]

_HERE = os.path.dirname(os.path.abspath(__file__))

DEPS_DIR    = os.path.join(_HERE, "dependencies")
MODELS_DIR  = DEPS_DIR
ESPEAK_DATA = os.path.join(DEPS_DIR, "espeak-ng-data")

if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

import tts_engine as _cpp

_engine = _cpp.TTSEngine(MODELS_DIR, ESPEAK_DATA)


def synthesize(
    text: str,
    language: Language = "polish",
    gender: Gender = "female",
    volume_db: float = 0.0,
    noise_scale: float = 0.667,
    length_scale: float = 1.0,
    bitrate_kbps: int = 24,
) -> bytes:
    """
    Synthesize text to OGG/Opus bytes entirely in C++.

    Args:
        text:          UTF-8 text to speak.
        language:      "polish", "english", or "german".
        gender:        "female" or "male".
        volume_db:     Volume shift in dB (0 = unchanged, +5 = louder, -5 = quieter).
        noise_scale:   Phoneme variation 0.0–1.0 (default 0.667).
                       Higher = more natural/expressive, too high = unstable.
        length_scale:  Speaking rate multiplier (default 1.0, 0.8 = faster, 1.3 = slower).
        bitrate_kbps:  Opus bitrate in kbps (16 = smallest, 24 = default, 48 = transparent).

    Returns:
        OGG/Opus audio as bytes, ready to stream.
    """
    return _engine.synthesize(
        text, language, gender, volume_db, noise_scale, length_scale, bitrate_kbps
    )


if __name__ == "__main__":
    print("[tts] Using C++ backend — piper inference + libopus encoding")

    cases = [
        ("standard",           dict(language="polish", gender="female")),
        ("louder (+5 dB)",     dict(language="polish", gender="female", volume_db=5.0)),
        ("expressive",         dict(language="polish", gender="female", noise_scale=0.9)),
        ("slow",               dict(language="polish", gender="female", length_scale=1.4)),
        ("fast + low bitrate", dict(language="polish", gender="female",
                                    length_scale=0.8, bitrate_kbps=16)),
    ]

    text = "Pociąg do stacji Gdańsk Śródmieście odjeżdża z peronu numer 3."

    for label, kwargs in cases:
        opus = synthesize(text, **kwargs)
        path = os.path.join(_HERE, f"test_{label.replace(' ', '_').replace('+', 'plus')}.opus")
        with open(path, "wb") as f:
            f.write(opus)
        print(f"  [{label}] {len(opus):>7} bytes → {os.path.basename(path)}")
```

---

## Dependencies

| Library | Role |
|---|---|
| [piper](https://github.com/rhasspy/piper) | TTS engine — phonemization + ONNX inference |
| [piper-phonemize](https://github.com/rhasspy/piper-phonemize) | Text → phonemes via espeak-ng |
| [onnxruntime](https://github.com/microsoft/onnxruntime) | Neural network inference |
| [libopus](https://opus-codec.org) | Opus audio encoding |
| [libogg](https://xiph.org/ogg) | OGG container muxing |
| [pybind11](https://github.com/pybind/pybind11) | C++/Python bindings |

---

## License

MIT
