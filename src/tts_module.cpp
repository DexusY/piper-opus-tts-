// piper TTS → OGG/Opus, exposed to Python via pybind11.

#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include <piper.hpp>

#include <ogg/ogg.h>
#include <opus/opus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace py = pybind11;

// --- Voice registry ---

using VoiceKey   = std::pair<std::string, std::string>;
using ModelPaths = std::pair<std::string, std::string>;

struct VoiceKeyHash {
    size_t operator()(const VoiceKey& k) const noexcept {
        size_t h1 = std::hash<std::string>{}(k.first);
        size_t h2 = std::hash<std::string>{}(k.second);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

static const std::unordered_map<VoiceKey, ModelPaths, VoiceKeyHash> VOICE_MODELS = {
    {{"polish",  "female"}, {"pl_PL-gosia-medium.onnx",     "pl_PL-gosia-medium.onnx.json"}},
    {{"polish",  "male"},   {"pl_PL-darkman-medium.onnx",   "pl_PL-darkman-medium.onnx.json"}},
    {{"english", "female"}, {"en_US-amy-medium.onnx",       "en_US-amy-medium.onnx.json"}},
    {{"english", "male"},   {"en_US-ryan-medium.onnx",      "en_US-ryan-medium.onnx.json"}},
    {{"german",  "female"}, {"de_DE-eva_k-x_low.onnx",      "de_DE-eva_k-x_low.onnx.json"}},
    {{"german",  "male"},   {"de_DE-thorsten-medium.onnx",  "de_DE-thorsten-medium.onnx.json"}},
};

// --- DSP ---

// Linear is good enough for 22050→24000 on speech — not worth a libsamplerate dep.
static void resample_linear(const int16_t* src, size_t src_len,
                            uint32_t src_rate, uint32_t dst_rate,
                            std::vector<int16_t>& dst)
{
    if (src_rate == dst_rate) {
        dst.assign(src, src + src_len);
        return;
    }
    const double ratio   = static_cast<double>(dst_rate) / src_rate;
    const size_t dst_len = static_cast<size_t>(src_len * ratio);
    dst.resize(dst_len);
    for (size_t i = 0; i < dst_len; ++i) {
        const double pos  = i / ratio;
        const size_t j    = static_cast<size_t>(pos);
        const double frac = pos - j;
        const int16_t a = src[j];
        const int16_t b = (j + 1 < src_len) ? src[j + 1] : a;
        dst[i] = static_cast<int16_t>(a + frac * (b - a));
    }
}

static inline void apply_gain_inplace(int16_t* samples, size_t n, float gain)
{
    if (gain == 1.0f) return;
    for (size_t i = 0; i < n; ++i) {
        float v = samples[i] * gain;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        samples[i] = static_cast<int16_t>(v);
    }
}

// Opus only accepts {8000, 12000, 16000, 24000, 48000}. Pick the smallest one
// that doesn't downsample piper's output (16k stays 16k; 22050 goes to 24k).
static uint32_t pickOpusRate(uint32_t src_rate) {
    if (src_rate <= 8000)   return 8000;
    if (src_rate <= 12000)  return 12000;
    if (src_rate <= 16000)  return 16000;
    if (src_rate <= 24000)  return 24000;
    return 48000;
}

// --- Thread-local Opus encoder (reused across calls on the same thread) ---

struct ThreadEncoder {
    OpusEncoder* enc  = nullptr;
    int          rate = 0;
    std::vector<int16_t> frame;     // padding scratch for the final partial frame
    std::vector<uint8_t> pkt;       // one Opus packet, ~4000 B max
    ThreadEncoder() { pkt.resize(4000); }
    ~ThreadEncoder() { if (enc) opus_encoder_destroy(enc); }
};

static thread_local ThreadEncoder t_enc;

static OpusEncoder* getThreadEncoder(int rate, int bitrate_bps, bool resetState)
{
    if (!t_enc.enc || t_enc.rate != rate) {
        if (t_enc.enc) opus_encoder_destroy(t_enc.enc);
        int err = 0;
        t_enc.enc = opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, &err);
        if (err != OPUS_OK || !t_enc.enc) {
            t_enc.enc  = nullptr;
            t_enc.rate = 0;
            throw std::runtime_error(
                std::string("opus_encoder_create: ") + opus_strerror(err));
        }
        t_enc.rate = rate;

        // VOIP/announcement tuning: complexity 5 ≈ half the CPU of 10 for
        // negligible quality loss on speech. DTX and FEC off — we never drop
        // packets in-process.
        opus_encoder_ctl(t_enc.enc, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(t_enc.enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(t_enc.enc, OPUS_SET_VBR(1));
        opus_encoder_ctl(t_enc.enc, OPUS_SET_VBR_CONSTRAINT(1));
        opus_encoder_ctl(t_enc.enc, OPUS_SET_DTX(0));
        opus_encoder_ctl(t_enc.enc, OPUS_SET_INBAND_FEC(0));
    } else if (resetState) {
        opus_encoder_ctl(t_enc.enc, OPUS_RESET_STATE);
    }
    opus_encoder_ctl(t_enc.enc, OPUS_SET_BITRATE(bitrate_bps));
    return t_enc.enc;
}

// --- Ogg serial-number generator (thread-safe; replaces std::rand) ---

static int next_serial() {
    static thread_local std::mt19937 rng{
        static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) ^
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&rng))
    };
    return static_cast<int>(rng());
}

// --- Ogg/Opus muxer (state survives across piper's per-sentence callbacks) ---

struct OggOpusEncoder {
    ogg_stream_state os{};
    OpusEncoder*     enc          = nullptr;
    int              rate         = 0;
    int              frameSz      = 0;       // 20 ms in samples at `rate`
    int              granuleScale = 1;       // multiplier → 48 kHz granule units
    int              preSkip48    = 0;       // encoder lookahead in 48 kHz samples
    int64_t          granule48    = 0;       // running granulepos at 48 kHz
    int64_t          packetno     = 0;
    bool             open         = false;
    std::vector<int16_t> carry;              // leftover PCM, not yet a full frame

    void init(int sample_rate, int bitrate_bps) {
        rate         = sample_rate;
        frameSz      = sample_rate / 50;     // 20 ms
        granuleScale = 48000 / sample_rate;  // RFC 7845: granulepos units are 48 kHz
        enc          = getThreadEncoder(sample_rate, bitrate_bps, /*reset=*/true);

        opus_int32 lookahead = 0;
        opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
        // Lookahead comes back at the encoder rate; the pre-skip header wants 48 kHz.
        preSkip48 = static_cast<int>(lookahead) * granuleScale;

        ogg_stream_init(&os, next_serial());
        open      = true;
        granule48 = 0;
        packetno  = 0;
        carry.clear();
    }

    void writeHeaders(std::vector<uint8_t>& out) {
        // OpusHead — see RFC 7845 §5.1 for the byte layout.
        uint8_t head[19] = {};
        std::memcpy(head, "OpusHead", 8);
        head[8]  = 1;        // version
        head[9]  = 1;        // channels (mono)
        head[10] =  preSkip48        & 0xFF;
        head[11] = (preSkip48 >> 8)  & 0xFF;
        head[12] =  rate        & 0xFF;
        head[13] = (rate >> 8)  & 0xFF;
        head[14] = (rate >> 16) & 0xFF;
        head[15] = (rate >> 24) & 0xFF;
        head[16] = 0; head[17] = 0;     // output gain
        head[18] = 0;                   // mapping family 0 (mono/stereo)

        ogg_packet op_head{};
        op_head.packet     = head;
        op_head.bytes      = 19;
        op_head.b_o_s      = 1;
        op_head.granulepos = 0;
        op_head.packetno   = packetno++;
        ogg_stream_packetin(&os, &op_head);
        flush(out);

        // OpusTags
        const char* vendor = "piper-opus-tts";
        const uint32_t vlen = static_cast<uint32_t>(std::strlen(vendor));
        std::vector<uint8_t> tags;
        tags.reserve(8 + 4 + vlen + 4);
        tags.insert(tags.end(), {'O','p','u','s','T','a','g','s'});
        for (int i = 0; i < 4; ++i) tags.push_back((vlen >> (8*i)) & 0xFF);
        tags.insert(tags.end(), vendor, vendor + vlen);
        for (int i = 0; i < 4; ++i) tags.push_back(0);  // zero user comments

        ogg_packet op_tags{};
        op_tags.packet     = tags.data();
        op_tags.bytes      = static_cast<long>(tags.size());
        op_tags.granulepos = 0;
        op_tags.packetno   = packetno++;
        ogg_stream_packetin(&os, &op_tags);
        flush(out);
    }

    void flush(std::vector<uint8_t>& out) {
        ogg_page og;
        while (ogg_stream_flush(&os, &og)) {
            out.insert(out.end(), og.header, og.header + og.header_len);
            out.insert(out.end(), og.body,   og.body   + og.body_len);
        }
    }
    void pageout(std::vector<uint8_t>& out) {
        ogg_page og;
        while (ogg_stream_pageout(&os, &og)) {
            out.insert(out.end(), og.header, og.header + og.header_len);
            out.insert(out.end(), og.body,   og.body   + og.body_len);
        }
    }

    void emitFrame(const int16_t* pcm, std::vector<uint8_t>& out, bool eos) {
        const int n = opus_encode(enc, pcm, frameSz,
                                  t_enc.pkt.data(),
                                  static_cast<opus_int32>(t_enc.pkt.size()));
        if (n < 0) {
            throw std::runtime_error(
                std::string("opus_encode: ") + opus_strerror(n));
        }
        // granulepos counts 48 kHz samples regardless of the encoder rate.
        granule48 += static_cast<int64_t>(frameSz) * granuleScale;
        ogg_packet op{};
        op.packet     = t_enc.pkt.data();
        op.bytes      = n;
        op.b_o_s      = 0;
        op.e_o_s      = eos ? 1 : 0;
        op.granulepos = granule48;
        op.packetno   = packetno++;
        ogg_stream_packetin(&os, &op);
        if (eos) flush(out);
        else      pageout(out);
    }

    // Chops `pcm` into 20 ms frames; anything left over goes into `carry`
    // for the next call. With finalize=true, also emits one padded frame
    // marked e_o_s to terminate the stream cleanly.
    void feed(const int16_t* pcm, size_t n,
              std::vector<uint8_t>& out, bool finalize)
    {
        if (!carry.empty()) {
            carry.insert(carry.end(), pcm, pcm + n);
            pcm = carry.data();
            n   = carry.size();
        }
        size_t pos = 0;
        while (pos + static_cast<size_t>(frameSz) <= n) {
            emitFrame(pcm + pos, out, /*eos=*/false);
            pos += frameSz;
        }
        const size_t remain = n - pos;
        if (finalize) {
            // Pad the last partial frame with silence and flag end-of-stream.
            t_enc.frame.assign(frameSz, 0);
            if (remain > 0) {
                std::memcpy(t_enc.frame.data(),
                            pcm + pos, remain * sizeof(int16_t));
            }
            emitFrame(t_enc.frame.data(), out, /*eos=*/true);
            carry.clear();
        } else {
            if (!carry.empty() && carry.data() == pcm) {
                carry.erase(carry.begin(), carry.begin() + pos);
            } else {
                carry.assign(pcm + pos, pcm + n);
            }
        }
    }

    void close() {
        if (open) {
            ogg_stream_clear(&os);
            open = false;
        }
    }
    ~OggOpusEncoder() { close(); }
};

// --- LRU output cache ---

class LruCache {
public:
    explicit LruCache(size_t cap = 256) : cap_(cap) {}

    void setCapacity(size_t cap) {
        std::lock_guard<std::mutex> g(m_);
        cap_ = cap;
        evict();
    }

    bool get(const std::string& key, std::vector<uint8_t>& out) {
        std::lock_guard<std::mutex> g(m_);
        auto it = map_.find(key);
        if (it == map_.end()) { ++misses_; return false; }
        order_.splice(order_.begin(), order_, it->second.it);
        out = it->second.data;
        ++hits_;
        return true;
    }

    void put(const std::string& key, std::vector<uint8_t> data) {
        std::lock_guard<std::mutex> g(m_);
        auto it = map_.find(key);
        if (it != map_.end()) {
            order_.splice(order_.begin(), order_, it->second.it);
            it->second.data = std::move(data);
            return;
        }
        order_.push_front(key);
        map_.emplace(key, Entry{order_.begin(), std::move(data)});
        evict();
    }

    void clear() {
        std::lock_guard<std::mutex> g(m_);
        order_.clear();
        map_.clear();
        hits_ = misses_ = 0;
    }

    py::dict stats() {
        std::lock_guard<std::mutex> g(m_);
        py::dict d;
        d["size"]     = map_.size();
        d["capacity"] = cap_;
        d["hits"]     = hits_;
        d["misses"]   = misses_;
        return d;
    }

private:
    struct Entry {
        std::list<std::string>::iterator it;
        std::vector<uint8_t>             data;
    };
    void evict() {
        while (map_.size() > cap_) {
            auto last = std::prev(order_.end());
            map_.erase(*last);
            order_.pop_back();
        }
    }
    size_t                                  cap_;
    std::list<std::string>                  order_;
    std::unordered_map<std::string, Entry>  map_;
    std::mutex                              m_;
    uint64_t                                hits_   = 0;
    uint64_t                                misses_ = 0;
};

// --- TTSEngine ---

class TTSEngine {
public:
    explicit TTSEngine(const std::string& modelsDir       = ".",
                       const std::string& eSpeakDataPath  = "espeak-ng-data",
                       size_t             cache_capacity  = 256,
                       int                num_threads     = 1,
                       size_t             max_text_bytes  = 5000)
        : modelsDir_(modelsDir),
          maxTextBytes_(max_text_bytes),
          cache_(cache_capacity)
    {
        // Has to happen before piper::initialize / loadVoice — once the ONNX
        // session is created the thread-pool size is locked in. overwrite=0
        // keeps any value the host process already set.
        if (num_threads > 0) {
            std::string n = std::to_string(num_threads);
            ::setenv("OMP_NUM_THREADS", n.c_str(), 0);
            ::setenv("MKL_NUM_THREADS", n.c_str(), 0);
            ::setenv("OPENBLAS_NUM_THREADS", n.c_str(), 0);
            ::setenv("OMP_WAIT_POLICY", "PASSIVE", 0);
        }
        config_.eSpeakDataPath = eSpeakDataPath;
        piper::initialize(config_);
    }

    ~TTSEngine() {
        try { piper::terminate(config_); } catch (...) {}
    }

    void loadVoice(const std::string& language, const std::string& gender) {
        VoiceKey key{language, gender};
        {
            std::shared_lock<std::shared_mutex> rlock(voicesMx_);
            if (voices_.find(key) != voices_.end()) return;
        }
        std::unique_lock<std::shared_mutex> wlock(voicesMx_);
        if (voices_.find(key) != voices_.end()) return;

        auto it = VOICE_MODELS.find(key);
        if (it == VOICE_MODELS.end())
            throw std::runtime_error("Unknown voice: " + language + "/" + gender);

        auto entry = std::make_unique<VoiceEntry>();
        std::optional<piper::SpeakerId> speakerId;
        piper::loadVoice(config_,
                         modelsDir_ + "/" + it->second.first,
                         modelsDir_ + "/" + it->second.second,
                         entry->voice, speakerId, /*useCuda=*/false);
        voices_.emplace(std::move(key), std::move(entry));
    }

    void warmup(const std::string& language, const std::string& gender) {
        loadVoice(language, gender);
        // One tiny synthesis to JIT-warm the ONNX kernels and espeak-ng.
        try {
            (void)synthesizeImpl("a.", language, gender,
                                 0.0f, 0.667f, 1.0f, 24,
                                 /*use_cache=*/false, nullptr);
        } catch (...) { /* best-effort */ }
    }

    py::bytes synthesize(const std::string& text,
                         const std::string& language    = "polish",
                         const std::string& gender      = "female",
                         float volume_db                = 0.0f,
                         float noise_scale              = 0.667f,
                         float length_scale             = 1.0f,
                         int   bitrate_kbps             = 24,
                         bool  use_cache                = true)
    {
        validateInput(text, bitrate_kbps);
        auto bytes = synthesizeImpl(text, language, gender,
                                    volume_db, noise_scale, length_scale,
                                    bitrate_kbps, use_cache, nullptr);
        return py::bytes(reinterpret_cast<const char*>(bytes.data()),
                         bytes.size());
    }

    void synthesize_stream(const std::string& text,
                           std::function<void(py::bytes)> on_chunk,
                           const std::string& language    = "polish",
                           const std::string& gender      = "female",
                           float volume_db                = 0.0f,
                           float noise_scale              = 0.667f,
                           float length_scale             = 1.0f,
                           int   bitrate_kbps             = 24)
    {
        if (!on_chunk) throw std::invalid_argument("on_chunk callback is required");
        validateInput(text, bitrate_kbps);
        synthesizeImpl(text, language, gender,
                       volume_db, noise_scale, length_scale,
                       bitrate_kbps, /*use_cache=*/false, &on_chunk);
    }

    py::dict cacheStats()              { return cache_.stats(); }
    void     cacheClear()              { cache_.clear(); }
    void     cacheCapacity(size_t n)   { cache_.setCapacity(n); }

private:
    struct VoiceEntry {
        piper::Voice voice;
        std::mutex   mx;     // synthesisConfig gets mutated per call — serialize
    };

    void validateInput(const std::string& text, int bitrate_kbps) const {
        if (text.empty())
            throw std::invalid_argument("text must not be empty");
        if (text.size() > maxTextBytes_)
            throw std::invalid_argument(
                "text exceeds max_text_bytes (" +
                std::to_string(maxTextBytes_) + ")");
        if (bitrate_kbps < 6 || bitrate_kbps > 510)
            throw std::invalid_argument("bitrate_kbps out of [6, 510]");
    }

    static std::string makeKey(const std::string& text,
                               const std::string& language,
                               const std::string& gender,
                               float volume_db, float noise_scale,
                               float length_scale, int bitrate_kbps)
    {
        std::ostringstream oss;
        oss << language << '|' << gender
            << '|' << bitrate_kbps
            << '|' << volume_db
            << '|' << noise_scale
            << '|' << length_scale
            << '|' << text;
        return oss.str();
    }

    std::vector<uint8_t> synthesizeImpl(
        const std::string& text,
        const std::string& language,
        const std::string& gender,
        float  volume_db,
        float  noise_scale,
        float  length_scale,
        int    bitrate_kbps,
        bool   use_cache,
        std::function<void(py::bytes)>* on_chunk /* optional */)
    {
        std::string ck;
        if (use_cache && !on_chunk) {
            ck = makeKey(text, language, gender, volume_db, noise_scale,
                         length_scale, bitrate_kbps);
            std::vector<uint8_t> cached;
            if (cache_.get(ck, cached)) return cached;
        }

        loadVoice(language, gender);
        VoiceEntry* ve = nullptr;
        {
            std::shared_lock<std::shared_mutex> rlock(voicesMx_);
            ve = voices_.at({language, gender}).get();
        }

        std::vector<uint8_t> ogg;
        // Roughly 2 s of audio up front to avoid the first few reallocs.
        ogg.reserve(static_cast<size_t>(bitrate_kbps) * 250);

        // Holds the voice across the synthesisConfig writes and textToAudio call.
        std::lock_guard<std::mutex> vlock(ve->mx);
        ve->voice.synthesisConfig.noiseScale  = noise_scale;
        ve->voice.synthesisConfig.lengthScale = length_scale;

        const uint32_t srcRate = static_cast<uint32_t>(
            ve->voice.synthesisConfig.sampleRate);
        const uint32_t outRate = pickOpusRate(srcRate);
        const float    gain    = (volume_db == 0.0f)
                                 ? 1.0f
                                 : std::pow(10.0f, volume_db / 20.0f);

        OggOpusEncoder oo;
        oo.init(static_cast<int>(outRate), bitrate_kbps * 1000);
        oo.writeHeaders(ogg);

        std::vector<int16_t> sentencePcm;
        std::vector<int16_t> resampled;
        piper::SynthesisResult result{};
        std::exception_ptr cb_err;

        // Drop the GIL for the whole inference path; we only reacquire it
        // inside the callback when we have to hand bytes back to Python.
        py::gil_scoped_release release;

        // piper fills sentencePcm, fires audioCb, then clears it. Per sentence
        // we resample, apply gain, encode and (maybe) flush a page.
        auto audioCb = [&]() {
            if (cb_err) return;
            try {
                if (sentencePcm.empty()) return;

                int16_t* src = sentencePcm.data();
                size_t   n   = sentencePcm.size();

                if (srcRate != outRate) {
                    resample_linear(src, n, srcRate, outRate, resampled);
                    src = resampled.data();
                    n   = resampled.size();
                }
                apply_gain_inplace(src, n, gain);

                oo.feed(src, n, ogg, /*finalize=*/false);

                if (on_chunk && !ogg.empty()) {
                    py::gil_scoped_acquire gil;
                    (*on_chunk)(py::bytes(
                        reinterpret_cast<const char*>(ogg.data()),
                        ogg.size()));
                    ogg.clear();
                }
            } catch (...) {
                cb_err = std::current_exception();
            }
        };

        try {
            piper::textToAudio(config_, ve->voice, text,
                               sentencePcm, result, audioCb);
        } catch (...) {
            oo.close();
            throw;
        }
        if (cb_err) {
            oo.close();
            std::rethrow_exception(cb_err);
        }

        // Flush whatever's left in `carry` and emit the e_o_s frame.
        oo.feed(nullptr, 0, ogg, /*finalize=*/true);
        oo.close();

        // Hand the final tail to the streaming consumer.
        if (on_chunk && !ogg.empty()) {
            py::gil_scoped_acquire gil;
            (*on_chunk)(py::bytes(
                reinterpret_cast<const char*>(ogg.data()), ogg.size()));
            ogg.clear();
        }

        if (use_cache && !on_chunk && !ogg.empty()) {
            cache_.put(ck, ogg);
        }
        return ogg;
    }

    std::string                                                          modelsDir_;
    size_t                                                               maxTextBytes_;
    piper::PiperConfig                                                   config_;
    std::unordered_map<VoiceKey, std::unique_ptr<VoiceEntry>, VoiceKeyHash> voices_;
    std::shared_mutex                                                    voicesMx_;
    LruCache                                                             cache_;
};

// --- pybind11 bindings ---

PYBIND11_MODULE(tts_engine, m) {
    m.doc() = "piper TTS encoded to OGG/Opus bytes";

    py::class_<TTSEngine>(m, "TTSEngine")
        .def(py::init<const std::string&, const std::string&,
                      size_t, int, size_t>(),
             py::arg("models_dir")        = ".",
             py::arg("espeak_data_path")  = "espeak-ng-data",
             py::arg("cache_capacity")    = 256,
             py::arg("num_threads")       = 1,
             py::arg("max_text_bytes")    = 5000)
        .def("load_voice", &TTSEngine::loadVoice,
             py::arg("language"), py::arg("gender"),
             "Preload a voice (idempotent, thread-safe).")
        .def("warmup", &TTSEngine::warmup,
             py::arg("language"), py::arg("gender"),
             "Load voice and run a tiny synth to JIT-warm ONNX kernels.")
        .def("synthesize", &TTSEngine::synthesize,
             py::arg("text"),
             py::arg("language")     = "polish",
             py::arg("gender")       = "female",
             py::arg("volume_db")    = 0.0f,
             py::arg("noise_scale")  = 0.667f,
             py::arg("length_scale") = 1.0f,
             py::arg("bitrate_kbps") = 24,
             py::arg("use_cache")    = true,
             "Synthesize text. Returns OGG/Opus bytes. "
             "Result is cached when use_cache=True.")
        .def("synthesize_stream", &TTSEngine::synthesize_stream,
             py::arg("text"), py::arg("on_chunk"),
             py::arg("language")     = "polish",
             py::arg("gender")       = "female",
             py::arg("volume_db")    = 0.0f,
             py::arg("noise_scale")  = 0.667f,
             py::arg("length_scale") = 1.0f,
             py::arg("bitrate_kbps") = 24,
             "Stream OGG/Opus pages: on_chunk(bytes) is invoked per piper "
             "sentence + once for final tail. Bypasses cache.")
        .def("cache_stats",    &TTSEngine::cacheStats,
             "Return {size, capacity, hits, misses}.")
        .def("cache_clear",    &TTSEngine::cacheClear)
        .def("cache_capacity", &TTSEngine::cacheCapacity, py::arg("size"));
}
