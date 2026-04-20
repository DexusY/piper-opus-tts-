# piper-opus-tts

In-memory text-to-speech engine built for **real-time and latency-sensitive systems**. Synthesizes speech directly to OGG/Opus bytes inside a C++ module — no temp files, no subprocess calls, no intermediate WAV handed to Python.

Designed to be dropped into WebSocket servers, VoIP pipelines, announcement systems, and any application where audio must be ready to stream within a consistent, tight latency budget.

---

## How it works

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

Everything from phonemization to Opus encoding happens in a single C++ call. The GIL is released during inference so multiple threads synthesize concurrently without blocking each other.

---

## Why this matters for real-time systems

| Approach | Latency per call | Notes |
|---|---|---|
| Python `piper-tts` + `pydub` + ffmpeg subprocess | ~300–700 ms | Subprocess fork + pipe + temp file per call |
| C++ piper + Python subprocess to ffmpeg | ~200–500 ms | No temp file but subprocess overhead remains |
| **This engine** | **~150–400 ms** | Single C++ call, no subprocess, no disk I/O |

The dominant cost is ONNX neural network inference, which scales with text length. Everything else (phonemization, resampling, Opus encoding) adds less than 10 ms total.

For streaming use cases, call `synthesize()` in a thread pool — the GIL release inside the C++ module means Python threads genuinely run in parallel during inference.

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

Voice models are downloaded from [rhasspy/piper-voices on Hugging Face](https://huggingface.co/rhasspy/piper-voices) and placed in `dependencies/`. Each voice requires two files: `.onnx` (model weights) and `.onnx.json` (config).

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

`build.sh` handles everything automatically:
- Clones piper C++ source
- Downloads pre-built piper-phonemize (headers + runtime libs)
- Downloads onnxruntime and spdlog headers
- Compiles `tts_module.cpp` into `tts_engine.cpython-*.so`

**3. Download voice models**

Place `.onnx` and `.onnx.json` pairs into `dependencies/`. Example for Polish female voice:

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

Native Windows is not supported — piper-phonemize is distributed as a Linux x86_64 binary and the build system uses Linux tooling. WSL2 runs the engine at native Linux performance.

**1. Install WSL2**

```powershell
wsl --install
```

Restart, then open the Ubuntu terminal that appears.

**2. Inside WSL2, follow the Linux instructions exactly**

```bash
sudo apt-get update
sudo apt-get install -y build-essential git python3-dev libopus-dev libogg-dev pkg-config

git clone https://github.com/DexusY/piper-opus-tts.git
cd piper-opus-tts
bash build/build.sh
```

Download voice models and run `python main.py` as described in the Linux section.

**Accessing files from Windows:** your WSL2 home directory is visible in Explorer at `\\wsl$\Ubuntu\home\<username>\`.

---

## Usage

```python
from main import synthesize

# Minimal — returns OGG/Opus bytes
opus: bytes = synthesize("Hello, world!", language="english", gender="female")

# Stream it directly over a WebSocket, HTTP response, etc.
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

Returns `bytes` — OGG/Opus audio ready to write to disk or stream over any transport.

---

## Example usage (`main.py`)

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
