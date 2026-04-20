// pybind11 module: piper TTS + libopus/libogg -> OGG/Opus bytes
// WAV is internal only, never returned to Python
// build: see CMakeLists.txt

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <piper.hpp>

#include <ogg/ogg.h>
#include <opus/opus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

// voice model registry

using VoiceKey   = std::pair<std::string, std::string>;
using ModelPaths = std::pair<std::string, std::string>;

static const std::map<VoiceKey, ModelPaths> VOICE_MODELS = {
    {{"polish",  "female"}, {"pl_PL-gosia-medium.onnx",    "pl_PL-gosia-medium.onnx.json"}},
    {{"polish",  "male"},   {"pl_PL-darkman-medium.onnx",  "pl_PL-darkman-medium.onnx.json"}},
    {{"english", "female"}, {"en_US-amy-medium.onnx",      "en_US-amy-medium.onnx.json"}},
    {{"english", "male"},   {"en_US-ryan-medium.onnx",     "en_US-ryan-medium.onnx.json"}},
    {{"german",  "female"}, {"de_DE-eva_k-x_low.onnx",    "de_DE-eva_k-x_low.onnx.json"}},
    {{"german",  "male"},   {"de_DE-thorsten-medium.onnx", "de_DE-thorsten-medium.onnx.json"}},
};

// PCM helpers

// Linear interpolation resample — good enough for speech, no extra deps.
static std::vector<int16_t> resample(const std::vector<int16_t>& src,
                                     uint32_t src_rate, uint32_t dst_rate)
{
    if (src_rate == dst_rate) return src;
    double ratio = static_cast<double>(dst_rate) / src_rate;
    size_t dst_len = static_cast<size_t>(src.size() * ratio);
    std::vector<int16_t> dst(dst_len);
    for (size_t i = 0; i < dst_len; ++i) {
        double pos = i / ratio;
        size_t j   = static_cast<size_t>(pos);
        double frac = pos - j;
        int16_t a = src[j];
        int16_t b = (j + 1 < src.size()) ? src[j + 1] : a;
        dst[i] = static_cast<int16_t>(a + frac * (b - a));
    }
    return dst;
}

static void apply_volume_db(std::vector<int16_t>& samples, float db)
{
    if (db == 0.0f) return;
    float gain = std::pow(10.0f, db / 20.0f);
    for (auto& s : samples) {
        float v = s * gain;
        s = static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, v)));
    }
}

// OGG/Opus encoder

static std::vector<uint8_t> encode_opus_ogg(const std::vector<int16_t>& pcm48k,
                                             int bitrate_bps)
{
    static constexpr int SAMPLE_RATE = 48000;
    static constexpr int CHANNELS    = 1;
    static constexpr int FRAME_SIZE  = 960;  // 20 ms at 48 kHz

    int err;
    OpusEncoder* enc = opus_encoder_create(SAMPLE_RATE, CHANNELS,
                                           OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK)
        throw std::runtime_error(std::string("opus_encoder_create: ") + opus_strerror(err));

    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate_bps));

    ogg_stream_state os;
    ogg_stream_init(&os, static_cast<int>(std::rand()));

    std::vector<uint8_t> output;

    auto flush_pages = [&]() {
        ogg_page og;
        while (ogg_stream_flush(&os, &og)) {
            output.insert(output.end(), og.header, og.header + og.header_len);
            output.insert(output.end(), og.body,   og.body   + og.body_len);
        }
    };
    auto write_pages = [&]() {
        ogg_page og;
        while (ogg_stream_pageout(&os, &og)) {
            output.insert(output.end(), og.header, og.header + og.header_len);
            output.insert(output.end(), og.body,   og.body   + og.body_len);
        }
    };

    // OpusHead
    uint8_t head[19] = {};
    std::memcpy(head, "OpusHead", 8);
    head[8]  = 1;           // version
    head[9]  = CHANNELS;
    // pre-skip = 0 (little-endian uint16)
    head[10] = 0; head[11] = 0;
    // input sample rate (informational)
    head[12] = SAMPLE_RATE & 0xFF;
    head[13] = (SAMPLE_RATE >> 8)  & 0xFF;
    head[14] = (SAMPLE_RATE >> 16) & 0xFF;
    head[15] = (SAMPLE_RATE >> 24) & 0xFF;
    // output gain = 0, channel mapping family = 0
    head[16] = 0; head[17] = 0; head[18] = 0;

    ogg_packet op_head;
    op_head.packet     = head;
    op_head.bytes      = 19;
    op_head.b_o_s      = 1;
    op_head.e_o_s      = 0;
    op_head.granulepos = 0;
    op_head.packetno   = 0;
    ogg_stream_packetin(&os, &op_head);
    flush_pages();

    // OpusTags (minimal — no user comments)
    const char* vendor     = "piper-tts-engine";
    uint32_t    vendor_len = static_cast<uint32_t>(std::strlen(vendor));
    std::vector<uint8_t> tags;
    tags.insert(tags.end(), {'O','p','u','s','T','a','g','s'});
    for (int i = 0; i < 4; ++i) tags.push_back((vendor_len >> (8*i)) & 0xFF);
    tags.insert(tags.end(), vendor, vendor + vendor_len);
    for (int i = 0; i < 4; ++i) tags.push_back(0);  // user comment list length = 0

    ogg_packet op_tags;
    op_tags.packet     = tags.data();
    op_tags.bytes      = static_cast<long>(tags.size());
    op_tags.b_o_s      = 0;
    op_tags.e_o_s      = 0;
    op_tags.granulepos = 0;
    op_tags.packetno   = 1;
    ogg_stream_packetin(&os, &op_tags);
    flush_pages();

    // audio frames
    std::vector<uint8_t> pkt_buf(4000);
    int64_t granule   = 0;
    int64_t packetno  = 2;
    size_t  pos       = 0;
    size_t  total     = pcm48k.size();

    while (pos < total) {
        std::vector<int16_t> frame(FRAME_SIZE, 0);
        size_t copy = std::min(static_cast<size_t>(FRAME_SIZE), total - pos);
        std::copy(pcm48k.begin() + pos, pcm48k.begin() + pos + copy, frame.begin());
        pos += copy;

        int nbytes = opus_encode(enc, frame.data(), FRAME_SIZE,
                                 pkt_buf.data(), static_cast<opus_int32>(pkt_buf.size()));
        if (nbytes < 0) {
            opus_encoder_destroy(enc);
            ogg_stream_clear(&os);
            throw std::runtime_error(std::string("opus_encode: ") + opus_strerror(nbytes));
        }

        granule += FRAME_SIZE;
        bool last = (pos >= total);

        ogg_packet op;
        op.packet     = pkt_buf.data();
        op.bytes      = nbytes;
        op.b_o_s      = 0;
        op.e_o_s      = last ? 1 : 0;
        op.granulepos = granule;
        op.packetno   = packetno++;
        ogg_stream_packetin(&os, &op);

        if (last) flush_pages();
        else       write_pages();
    }

    opus_encoder_destroy(enc);
    ogg_stream_clear(&os);
    return output;
}

// TTSEngine

class TTSEngine {
public:
    explicit TTSEngine(const std::string& modelsDir     = ".",
                       const std::string& eSpeakDataPath = "espeak-ng-data")
        : modelsDir_(modelsDir)
    {
        config_.eSpeakDataPath = eSpeakDataPath;
        piper::initialize(config_);
    }

    ~TTSEngine() { piper::terminate(config_); }

    void loadVoice(const std::string& language, const std::string& gender) {
        auto key = VoiceKey{language, gender};
        if (voices_.count(key)) return;

        auto it = VOICE_MODELS.find(key);
        if (it == VOICE_MODELS.end())
            throw std::runtime_error("Unknown voice: " + language + "/" + gender);

        piper::Voice voice;
        std::optional<piper::SpeakerId> speakerId;
        piper::loadVoice(config_,
                         modelsDir_ + "/" + it->second.first,
                         modelsDir_ + "/" + it->second.second,
                         voice, speakerId, /*useCuda=*/false);
        voices_.emplace(key, std::move(voice));
    }

    // noise_scale > ~1.0 tends to produce artifacts; length_scale < 1.0 = faster
    py::bytes synthesize(const std::string& text,
                         const std::string& language    = "polish",
                         const std::string& gender      = "female",
                         float volume_db                = 0.0f,
                         float noise_scale              = 0.667f,
                         float length_scale             = 1.0f,
                         int   bitrate_kbps             = 24)
    {
        auto key = VoiceKey{language, gender};
        if (!voices_.count(key)) loadVoice(language, gender);

        std::vector<uint8_t> ogg;
        {
            py::gil_scoped_release release;

            auto& voice = voices_.at(key);
            voice.synthesisConfig.noiseScale   = noise_scale;
            voice.synthesisConfig.lengthScale  = length_scale;

            std::vector<int16_t> pcm;
            piper::SynthesisResult result;
            piper::textToAudio(config_, voice, text, pcm, result, nullptr);

            uint32_t src_rate = voice.synthesisConfig.sampleRate;
            auto pcm48k = resample(pcm, src_rate, 48000);
            apply_volume_db(pcm48k, volume_db);
            ogg = encode_opus_ogg(pcm48k, bitrate_kbps * 1000);
        }

        return py::bytes(reinterpret_cast<const char*>(ogg.data()), ogg.size());
    }

private:
    std::string                      modelsDir_;
    piper::PiperConfig               config_;
    std::map<VoiceKey, piper::Voice> voices_;
};

// pybind11 bindings

PYBIND11_MODULE(tts_engine, m) {
    m.doc() = "Piper TTS — synthesizes text directly to OGG/Opus bytes";

    py::class_<TTSEngine>(m, "TTSEngine")
        .def(py::init<const std::string&, const std::string&>(),
             py::arg("models_dir")        = ".",
             py::arg("espeak_data_path")  = "espeak-ng-data")
        .def("load_voice", &TTSEngine::loadVoice,
             py::arg("language"), py::arg("gender"),
             "Preload a voice to avoid first-call latency.")
        .def("synthesize", &TTSEngine::synthesize,
             py::arg("text"),
             py::arg("language")     = "polish",
             py::arg("gender")       = "female",
             py::arg("volume_db")    = 0.0f,
             py::arg("noise_scale")  = 0.667f,
             py::arg("length_scale") = 1.0f,
             py::arg("bitrate_kbps") = 24,
             "Synthesize text and return OGG/Opus bytes. WAV is internal only.");
}
