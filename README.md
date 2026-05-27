# piper-opus-tts

A C++ TTS engine that takes text and gives you back OGG/Opus bytes. The
whole pipeline — phonemize, run the neural model, resample, encode, mux —
lives in one process. Python gets the encoded bytes directly: no temp
files, no ffmpeg subprocess, no WAV in the middle.

Built with WebSocket servers, VoIP stacks, and announcement systems in
mind, and for embedded devices that decode Opus straight off a TCP socket.

---

## What's inside

```
text + voice params
        │
        ▼
[ C++  tts_engine.so ]
        │
        ├─ piper::textToAudio()     text → phonemes (espeak-ng) → ONNX → int16 PCM
        │                            (one sentence at a time)
        │
        ├─ resample_linear()         only when needed: 22050 → 24000 Hz
        │                            (x_low 16 kHz voices skip this entirely)
        │
        ├─ apply_gain_inplace()      volume_db gain on PCM
        │
        └─ OggOpusEncoder            libopus → libogg pages
                                     (RFC 7845: granulepos at 48 kHz, pre-skip set)
                │
                ▼
        OGG/Opus bytes               returned in one shot, OR streamed per sentence
```

We encode at the model's native rate (24 kHz or 16 kHz) rather than
pushing everything through 48 kHz first. That avoids one lossy resample
and roughly halves the encoder's CPU. The GIL is dropped for the entire
inference + encode path, so Python threads doing `engine.synthesize(...)`
really do run in parallel.

---

## Latency

| Mode                                   | Per call         | Notes                                                             |
|----------------------------------------|------------------|-------------------------------------------------------------------|
| Python `piper-tts` + `pydub` + ffmpeg  | ~300–700 ms      | subprocess fork + temp file every call                            |
| C++ piper + Python subprocess to ffmpeg| ~200–500 ms      | no temp file, subprocess overhead stays                           |
| **This engine — full synth**           | **~130–200 ms**  | one C++ call, no subprocess, no disk I/O                          |
| **This engine — streaming first byte** | **~80–150 ms**   | first Opus page after the first sentence inferences               |
| **This engine — cache hit**            | **~10–50 µs**    | identical input, no re-synthesis                                  |

ONNX inference dominates and scales with text length. Phonemize,
resample, gain, and Opus encoding together come in under 10 ms.

---

## Project structure

```
├── main.py                         demo + reference Python wrapper (sync + stream + warmup)
├── output.opus                     produced by `python main.py`, gitignored
├── tts_engine.cpython-3*.so        built by build.sh, gitignored
├── LICENSE                         MIT
│
├── src/
│   └── tts_module.cpp              engine: piper + DSP + Opus muxer + LRU cache + streaming
│
├── build/
│   ├── CMakeLists.txt              Release default, -O3, LTO, hidden visibility, post-build strip
│   └── build.sh                    fetches/builds dependencies and compiles everything
│
└── dependencies/                   gitignored, populated by build.sh
    ├── *.onnx / *.onnx.json        voice models (download separately, see below)
    ├── espeak-ng-data/             phoneme data (extracted from piper-phonemize tarball)
    ├── include/                    piper / phonemize / onnxruntime / spdlog headers
    ├── lib/                        runtime shared libs (.so)
    ├── piper/                      cloned piper source (compiled into the .so)
    ├── local/                      libopus + libogg built from source if no system -dev pkgs
    └── .venv/                      pybind11 venv if PEP 668 blocks system pip (optional)
```

---

## Supported voices

| Language | Gender | Model file                       | Native rate |
|----------|--------|----------------------------------|-------------|
| Polish   | female | `pl_PL-gosia-medium.onnx`        | 22 050 Hz   |
| Polish   | male   | `pl_PL-darkman-medium.onnx`      | 22 050 Hz   |
| English  | female | `en_US-amy-medium.onnx`          | 22 050 Hz   |
| English  | male   | `en_US-ryan-medium.onnx`         | 22 050 Hz   |
| German   | female | `de_DE-eva_k-x_low.onnx`         | 16 000 Hz   |
| German   | male   | `de_DE-thorsten-medium.onnx`     | 22 050 Hz   |

All models come from
[rhasspy/piper-voices](https://huggingface.co/rhasspy/piper-voices). Each
voice needs both files — the `.onnx` weights and the `.onnx.json` config
— dropped into `dependencies/`.

22 050 Hz voices encode at 24 kHz (small upsample); 16 000 Hz voices
encode natively with no resample.

---

## Build

System packages, one-time:

```bash
sudo apt-get install -y \
    build-essential cmake git pkg-config python3-dev \
    libopus-dev libogg-dev pybind11-dev
```

Then:

```bash
bash build/build.sh
```

Missing `libopus-dev` / `libogg-dev`? The script builds them from source
into `dependencies/local/` (no sudo). If pip can't install pybind11 into
the system Python because of PEP 668, it sets up a venv at
`dependencies/.venv/` and uses that. Re-running is safe — anything
already in place is skipped.

The build drops `tts_engine.cpython-3X-x86_64-linux-gnu.so` into the
project root so `import tts_engine` works without messing with
`PYTHONPATH`.

### Voice models

Models aren't committed. Pull what you need into `dependencies/`:

```bash
cd dependencies
wget https://huggingface.co/rhasspy/piper-voices/resolve/main/pl/pl_PL/gosia/medium/pl_PL-gosia-medium.onnx
wget https://huggingface.co/rhasspy/piper-voices/resolve/main/pl/pl_PL/gosia/medium/pl_PL-gosia-medium.onnx.json
# same pattern for any other voice from the table above
```

### Windows (WSL2)

Native Windows isn't supported. Use WSL2 with Ubuntu:

```bash
wsl --install -d Ubuntu
# then, inside WSL:
sudo apt-get update && sudo apt-get install -y \
    build-essential cmake git pkg-config python3-dev \
    libopus-dev libogg-dev pybind11-dev wget
git clone <this repo> && cd piper-opus-tts-
bash build/build.sh
```

The `.so` behaves inside WSL2 exactly like it does on native Linux.

---

## Python API

One class, `TTSEngine`. Construct it once at startup and share it across
threads — that's how it's designed to be used.

```python
from tts_engine import TTSEngine

engine = TTSEngine(
    models_dir       = "dependencies",          # where .onnx / .onnx.json live
    espeak_data_path = "dependencies/espeak-ng-data",
    cache_capacity   = 256,                     # LRU size in entries
    num_threads      = 1,                       # ONNX intra-op threads
    max_text_bytes   = 5000,                    # rejects longer inputs
)
```

### `synthesize(...)` — full clip as `bytes`

```python
ogg_opus: bytes = engine.synthesize(
    text         = "Pociąg do stacji Gdańsk Śródmieście odjeżdża z peronu numer 3.",
    language     = "polish",      # polish | english | german
    gender       = "female",      # female | male
    volume_db    = 0.0,           # PCM gain, clipped at int16 range
    noise_scale  = 0.667,         # VITS noise; higher = more expressive (0.8 is a sweet spot)
    length_scale = 1.0,           # speed: <1 faster, >1 slower
    bitrate_kbps = 24,            # 6–510; 24 is fine for speech, 48 is transparent
    use_cache    = True,
)
```

### `synthesize_stream(...)` — Opus pages, sentence by sentence

Bypasses the cache. `on_chunk(bytes)` fires once per piper sentence,
plus one final call for the tail.

```python
def push_to_socket(chunk: bytes) -> None:
    writer.write(chunk)

engine.synthesize_stream(
    text       = long_text,
    on_chunk   = push_to_socket,
    language   = "polish",
    gender     = "female",
    # same volume / noise / length / bitrate args as synthesize()
)
```

### `warmup(language, gender)`

Loads the voice and runs a tiny throwaway synthesis. ONNX needs this to
JIT-warm its kernels; without warmup the first real request pays an
extra ~100 ms. Call it once per voice at server startup.

```python
engine.warmup("polish", "female")
```

### `load_voice(language, gender)`

Loads the voice without running inference. Idempotent and thread-safe.
Useful when you want the voice in memory but aren't ready to pay the
warmup cost yet.

### Cache controls

```python
engine.cache_stats()       # {'size': N, 'capacity': N, 'hits': N, 'misses': N}
engine.cache_clear()       # empty + reset stats
engine.cache_capacity(512) # resize; shrinks evict oldest entries
```

---

## Threading model

You can call `synthesize`, `synthesize_stream`, `warmup`, `load_voice`,
and the cache methods from any thread, concurrently.

- The GIL is released around the inference + encode path, so Python
  threads running TTS calls actually use multiple CPUs.
- Calls hitting the **same** voice serialize internally (piper mutates
  `synthesisConfig` per call, so they have to). Calls on **different**
  voices run in parallel.
- The voice registry sits behind a `shared_mutex`: reads are
  uncontended, and only the first load of a voice takes an exclusive
  lock.
- ONNX intra-op threading is pinned through `OMP_NUM_THREADS` /
  `MKL_NUM_THREADS` / `OPENBLAS_NUM_THREADS`. With `num_threads=1`
  (the default), each request uses one CPU and concurrency scales
  across workers — usually what you want for QPS. Raise it when you
  mostly serve one request at a time and care about single-call latency.

One ceiling you can't tune away: **espeak-ng phonemization is globally
serialized** inside piper-phonemize. It's normally <5 ms per call, but
at very high QPS with long inputs you'll queue there. The fix is
multiple processes, each with its own espeak.

---

## Streaming

`synthesize_stream(text, on_chunk, ...)` emits OGG/Opus bytes incrementally:

- One `on_chunk(bytes)` per piper sentence — the audio for that
  sentence, Opus-encoded and wrapped in Ogg pages.
- One final `on_chunk(bytes)` for the tail (drains the carry buffer
  and writes the e_o_s page).

Concatenate every chunk and you get a valid Ogg/Opus file. `granulepos`
is in 48 kHz units per RFC 7845, and the pre-skip header is filled in
from the encoder's own lookahead, so ffprobe and hardware decoders
agree on timestamps.

The win for a TCP push is TTFB: the first chunk goes out the moment
the first sentence finishes inference, not after the whole utterance.
For a 4-second clip that's roughly ~150 ms vs ~400 ms.

```python
async def stream_to_client(text, writer):
    def on_chunk(b):  # runs on the C++ worker thread; GIL is reacquired for you
        writer.write(b)
    await asyncio.to_thread(
        engine.synthesize_stream, text, on_chunk,
        language="polish", gender="female",
    )
```

The callback runs on the C++ worker thread, but pybind11 reacquires
the GIL before calling it. Touching Python objects (queues, locks,
`asyncio.Event`, etc.) inside `on_chunk` is fine.

---

## Output cache

A plain LRU. Identical calls short-circuit to the cached bytes. The
key is:

```
(text, language, gender, volume_db, noise_scale, length_scale, bitrate_kbps)
```

- Hit returns the cached `bytes` directly — no inference, no encoding,
  ~10–50 µs.
- Capacity is in entries, not bytes. Each entry is one full OGG/Opus
  payload, so budget roughly *bitrate × duration* per entry.
- Oldest entries evict on insert overflow.
- `synthesize_stream` skips the cache (output is fragmented anyway).

Announcement systems often hit >80% cache rates — "Train to X
departing platform Y" is the same string thousands of times a day.
For workloads like that the cache is by far the biggest latency and
CPU win.

---

## Wiring it into a server

The pattern is the same regardless of framework. pybind11 drops the
GIL during inference, so calls go into a thread pool and the event
loop stays responsive.

```python
import asyncio
from concurrent.futures import ThreadPoolExecutor

engine = TTSEngine(...)
for v in [("polish", "female"), ("english", "female")]:
    engine.warmup(*v)                       # pay the JIT cost up front

pool = ThreadPoolExecutor(max_workers=8)    # cap concurrency so OOM is impossible

async def synth(text, lang, gender) -> bytes:
    return await asyncio.get_event_loop().run_in_executor(
        pool, engine.synthesize, text, lang, gender,
    )
```

Knobs that actually matter in practice:

- **`num_threads=1` + a worker pool** parallelizes across requests,
  which is what you want for QPS. Raise `num_threads` when you mostly
  serve one request at a time and want the lowest single-call latency.
- **`cache_capacity`** big enough to hold your hot phrases. A few
  hundred entries is plenty for a station announcement system.
- **`max_text_bytes`** caps how long a single request can tie up a
  worker. Reject or split longer inputs upstream.
- **Warm every voice at startup**, not on first request, or the first
  user pays the JIT cost.
- **Use `synthesize_stream` for TCP pushes** so the decoder starts
  playing the moment the first sentence is ready.

---

## What's not done yet

The architecture and correctness work is in. What's left is
diminishing-returns stuff — worth doing for specific workloads, not
speculatively. Rough order of effort vs payoff:

- **Custom ONNX `SessionOptions`.** piper's bundled `loadModel` forces
  `GraphOptimizationLevel::ORT_DISABLE_ALL` and doesn't set intra-op
  thread counts, which is why we pin threading via env vars. Rebuilding
  the `Ort::Session` after `piper::loadVoice` (or patching
  `dependencies/piper/src/cpp/piper.cpp`) would let us own the session
  config. Maybe 5–10% inference speedup; cost is maintaining a piper diff.

- **Better resampler** (speexdsp polyphase, libsamplerate, or a
  hand-rolled windowed sinc) for the 22050 → 24000 step. Linear is
  fine at that 1.088× ratio, but polyphase would close the last small
  gap. Marginal audible improvement on speech — useful if you want
  to squeeze more perceived quality out of low bitrates.

- **Per-call Opus complexity.** The thread-local encoder is fixed at
  `OPUS_SET_COMPLEXITY(5)` — roughly half the CPU of complexity 10
  for negligible quality loss on speech. Trivial to expose as an
  argument if you want some calls to favor quality and others
  throughput.

- **SIMD gain loop.** `apply_gain_inplace` is a scalar loop; with
  `-O3 -ffast-math` the compiler should auto-vectorize. Hand-written
  SIMD would save sub-millisecond per call — only worth doing if
  profiling says it's actually hot.

Intentionally out of scope (these belong in the app layer):

- async/asyncio wrapper, bounded worker pool, graceful shutdown
- metrics export (Prometheus, OpenTelemetry, etc.)
- on-disk cache backed by the in-memory LRU
- pytest suite (verification today is `main.py` + `ffprobe`)

---

## Dependencies & licenses

Compiled into the .so or pulled at build time:

| Component         | License                                                       |
|-------------------|---------------------------------------------------------------|
| piper             | MIT — <https://github.com/rhasspy/piper>                      |
| piper-phonemize   | MIT — <https://github.com/rhasspy/piper-phonemize>            |
| espeak-ng         | GPL-3.0 (data + library, runtime dependency)                  |
| onnxruntime       | MIT — <https://github.com/microsoft/onnxruntime>              |
| spdlog            | MIT — <https://github.com/gabime/spdlog>                      |
| pybind11          | BSD-3-Clause                                                  |
| libopus           | BSD-3-Clause — <https://opus-codec.org/>                      |
| libogg            | BSD-3-Clause — <https://xiph.org/ogg/>                        |

Heads up: linking against GPL-3 espeak-ng has redistribution
implications. If you're shipping closed-source binaries, check the
espeak-ng license terms or swap the phonemizer.

---

## License

MIT. Full text in [LICENSE](LICENSE).
