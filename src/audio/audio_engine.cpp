#include "audio/audio_engine.h"

#include <objbase.h>
#include <windows.h>
#include <xapo.h>
#include <xaudio2.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <memory>
#include <unordered_map>

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include <stb_vorbis/stb_vorbis.c>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

namespace audio {

namespace {

constexpr u32 kMixRate = 48000;
constexpr int kStreamBuffers = 4;
constexpr double kStreamBufferSeconds = 0.5;

// ---- debug capture: an in-place XAPO on the mastering voice copies the final mix ----------
struct CaptureSink {
    std::vector<float> samples;  // interleaved stereo
    std::atomic<u64> frames{0};
    std::atomic<double>* start = nullptr;
    int channels = 2;

    void write(const float* data, u32 count, bool silent) {
        if (frames.load(std::memory_order_relaxed) == 0 && count) start->store(nowSeconds() - double(count) / kMixRate);
        for (u32 i = 0; i < count; ++i)
            for (int c = 0; c < 2; ++c) samples.push_back(silent ? 0.0f : data[size_t(i) * channels + std::min(c, channels - 1)]);
        frames.fetch_add(count, std::memory_order_release);
    }
};

// Implements IXAPO directly (no xapobase.lib: it drags in the dynamic CRT, the project uses /MT).
class __declspec(uuid("5b0c7a52-3f0e-4c8b-9a51-7e2f6d1c0a93")) CaptureXapo : public IXAPO {
public:
    explicit CaptureXapo(CaptureSink* sink) : sink_(sink) {}
    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** out) override {
        if (riid == __uuidof(IXAPO) || riid == __uuidof(IUnknown)) {
            *out = static_cast<IXAPO*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs_; }
    STDMETHOD_(ULONG, Release)() override {
        const ULONG r = --refs_;
        if (!r) delete this;
        return r;
    }
    // IXAPO
    STDMETHOD(GetRegistrationProperties)(XAPO_REGISTRATION_PROPERTIES** props) override {
        *props = static_cast<XAPO_REGISTRATION_PROPERTIES*>(XAPOAlloc(sizeof(XAPO_REGISTRATION_PROPERTIES)));
        if (!*props) return E_OUTOFMEMORY;
        **props = kProps;
        return S_OK;
    }
    STDMETHOD(IsInputFormatSupported)(const WAVEFORMATEX* output, const WAVEFORMATEX* requested, WAVEFORMATEX** supported) override {
        return formatSupported(output, requested, supported);
    }
    STDMETHOD(IsOutputFormatSupported)(const WAVEFORMATEX* input, const WAVEFORMATEX* requested, WAVEFORMATEX** supported) override {
        return formatSupported(input, requested, supported);
    }
    STDMETHOD(Initialize)(const void*, UINT32) override { return S_OK; }
    STDMETHOD_(void, Reset)() override {}
    STDMETHOD(LockForProcess)(UINT32 inCount, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS* in, UINT32,
                              const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS*) override {
        if (inCount) sink_->channels = in[0].pFormat->nChannels;
        return S_OK;
    }
    STDMETHOD_(void, UnlockForProcess)() override {}
    STDMETHOD_(void, Process)(UINT32, const XAPO_PROCESS_BUFFER_PARAMETERS* in, UINT32 outCount,
                              XAPO_PROCESS_BUFFER_PARAMETERS* out, BOOL) override {
        sink_->write(static_cast<const float*>(in[0].pBuffer), in[0].ValidFrameCount, in[0].BufferFlags == XAPO_BUFFER_SILENT);
        if (outCount) {
            out[0].BufferFlags = in[0].BufferFlags;
            out[0].ValidFrameCount = in[0].ValidFrameCount;
        }
    }
    STDMETHOD_(UINT32, CalcInputFrames)(UINT32 frames) override { return frames; }
    STDMETHOD_(UINT32, CalcOutputFrames)(UINT32 frames) override { return frames; }

private:
    static HRESULT formatSupported(const WAVEFORMATEX* other, const WAVEFORMATEX* requested, WAVEFORMATEX** supported) {
        // In place: the same format on both sides.
        const bool same = requested->nChannels == other->nChannels && requested->nSamplesPerSec == other->nSamplesPerSec &&
                          requested->wBitsPerSample == other->wBitsPerSample;
        if (same) return S_OK;
        if (supported) {
            *supported = static_cast<WAVEFORMATEX*>(XAPOAlloc(sizeof(WAVEFORMATEXTENSIBLE)));
            if (*supported) std::memcpy(*supported, other, other->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX));
        }
        return XAPO_E_FORMAT_UNSUPPORTED;
    }
    static const XAPO_REGISTRATION_PROPERTIES kProps;
    CaptureSink* sink_;
    std::atomic<ULONG> refs_{1};
};
const XAPO_REGISTRATION_PROPERTIES CaptureXapo::kProps = {
    __uuidof(CaptureXapo), L"minecraftCplusplus capture", L"", 1, 0,
    XAPO_FLAG_INPLACE_REQUIRED | XAPO_FLAG_INPLACE_SUPPORTED | XAPO_FLAG_CHANNELS_MUST_MATCH |
        XAPO_FLAG_FRAMERATE_MUST_MATCH | XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH | XAPO_FLAG_BUFFERCOUNT_MUST_MATCH,
    1, 1, 1, 1};

bool writeWav(const fs::path& path, const std::vector<float>& samples) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const u32 dataBytes = u32(samples.size() * sizeof(float));
    auto u32le = [&](u32 v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16le = [&](u16 v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32le(36 + dataBytes);
    f.write("WAVEfmt ", 8);
    u32le(16);
    u16le(3);  // IEEE float
    u16le(2);
    u32le(kMixRate);
    u32le(kMixRate * 8);
    u16le(8);
    u16le(32);
    f.write("data", 4);
    u32le(dataBytes);
    f.write(reinterpret_cast<const char*>(samples.data()), dataBytes);
    return bool(f);
}

std::string jsonEscape(std::string_view s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

}  // namespace

// ---- audio thread state -------------------------------------------------------------------
struct Engine::Impl {
    struct Pcm {
        short* data = nullptr;  // malloc'd by stb_vorbis
        u32 frames = 0;
        int channels = 0, rate = 0;
        bool failed = false;
    };
    struct Stream {
        std::vector<u8> file;
        stb_vorbis* vf = nullptr;
        int channels = 0, rate = 0;
        std::vector<short> bufs[kStreamBuffers];
        int next = 0;
        bool eof = false, loop = false;
        ~Stream() {
            if (vf) stb_vorbis_close(vf);
        }
    };
    struct Voice {
        IXAudio2SourceVoice* sv = nullptr;
        u32 key = 0;  // channels << 24 | rate
        int channels = 1;
        PlayRequest req;
        std::unique_ptr<Stream> stream;
        double started = 0;
        float gain = 0;         // current overall gain (for stealing)
        float matrix[4] = {-1, -1, -1, -1};
        float pitchSet = -1;
        bool draining = false;  // stopped; waiting for XAudio2 to release its buffers
    };
    struct LogEntry {
        double request, start;
        int event, file;
        PlayRequest req;
        float gain[2];
        double listener[3];
        float right[3];
    };

    IXAudio2* xa = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    std::vector<Pcm> pcm;  // by file index
    std::vector<Voice> voices;  // active (reserved; never reallocates)
    std::unordered_map<u32, std::vector<IXAudio2SourceVoice*>> pool;
    double listener[3] = {};
    float forward[3] = {0, 0, 1}, up[3] = {0, 1, 0}, right[3] = {-1, 0, 0};
    std::unique_ptr<CaptureSink> capture;
    std::vector<LogEntry> log;
    std::vector<std::pair<double, std::string>> marks;
    Stats stats;
    std::deque<int> preloadQueue;
};

bool Engine::start(const SoundBank& bank, const fs::path& capture) {
    bank_ = &bank;
    capturePath_ = capture;
    for (auto& g : gains_) g.store(1.0f);
    commands_.reserve(1024);
    finished_.reserve(1024);
    impl_ = new Impl;
    impl_->pcm.resize(bank.fileCount());
    impl_->voices.reserve(512);
    if (!capture.empty()) {
        impl_->capture = std::make_unique<CaptureSink>();
        impl_->capture->samples.reserve(size_t(kMixRate) * 2 * 180);
        impl_->capture->start = &captureStart_;
    }
    std::mutex startMutex;
    std::condition_variable startCv;
    int startState = 0;  // 1 ok, -1 failed
    thread_ = std::thread([&] {
        Impl& m = *impl_;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = SUCCEEDED(XAudio2Create(&m.xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) &&
                  SUCCEEDED(m.xa->CreateMasteringVoice(&m.master, 2, kMixRate, 0, nullptr, nullptr, AudioCategory_GameEffects));
        if (ok && m.capture) {
            CaptureXapo* xapo = new CaptureXapo(m.capture.get());
            XAUDIO2_EFFECT_DESCRIPTOR desc{static_cast<IXAPO*>(xapo), TRUE, 2};
            XAUDIO2_EFFECT_CHAIN chain{1, &desc};
            ok = SUCCEEDED(m.master->SetEffectChain(&chain));
            xapo->Release();
            if (!ok) LOG("audio: capture effect could not be attached");
        }
        {
            std::lock_guard lock(startMutex);
            startState = ok ? 1 : -1;
        }
        startCv.notify_one();
        if (ok) {
            running_ = true;
            run();
        }
        for (auto& v : m.voices) v.sv->DestroyVoice();
        m.voices.clear();
        for (auto& [k, list] : m.pool)
            for (auto* sv : list) sv->DestroyVoice();
        if (m.master) m.master->DestroyVoice();
        if (m.xa) m.xa->Release();
        m.master = nullptr, m.xa = nullptr;
        CoUninitialize();
    });
    std::unique_lock lock(startMutex);
    startCv.wait(lock, [&] { return startState != 0; });
    if (startState < 0) {
        LOG("audio: no audio output device (XAudio2 initialisation failed); sound disabled");
        thread_.join();
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    return true;
}

void Engine::stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    cv_.notify_one();
    thread_.join();
    running_ = false;
    Impl& m = *impl_;
    if (m.capture) {
        std::vector<float> samples = std::move(m.capture->samples);
        if (writeWav(capturePath_, samples))
            LOG("audio: captured %.2f s to %s", double(samples.size() / 2) / kMixRate, capturePath_.string().c_str());
        fs::path logPath = capturePath_;
        logPath.replace_extension(".json");
        std::ofstream f(logPath, std::ios::binary);
        const double t0 = captureStart_.load();
        f << "{\"rate\": " << kMixRate << ", \"channels\": 2, \"sounds\": [\n";
        for (size_t i = 0; i < m.log.size(); ++i) {
            const auto& e = m.log[i];
            char buf[2048];
            std::snprintf(buf, sizeof(buf),
                          "%s{\"request\": %.5f, \"start\": %.5f, \"event\": \"%s\", \"file\": \"%s\", \"path\": \"%s\", "
                          "\"category\": \"%s\", \"volume\": %.5f, \"pitch\": %.5f, \"attenuation\": %.3f, \"linear\": %s, "
                          "\"relative\": %s, \"stream\": %s, \"gain\": [%.5f, %.5f], \"pos\": [%.3f, %.3f, %.3f], "
                          "\"listener\": [%.3f, %.3f, %.3f], \"right\": [%.4f, %.4f, %.4f]}",
                          i ? ",\n" : "", e.request - t0, e.start - t0, jsonEscape(bank_->eventName(e.event)).c_str(),
                          jsonEscape(bank_->fileName(e.file)).c_str(),
                          jsonEscape(bank_->filePath(e.file).generic_string()).c_str(), categoryName(e.req.category),
                          e.req.volume, e.req.pitch, e.req.attenuation, e.req.linear ? "true" : "false",
                          e.req.relative ? "true" : "false", e.req.stream ? "true" : "false", e.gain[0], e.gain[1],
                          e.req.pos[0], e.req.pos[1], e.req.pos[2], e.listener[0], e.listener[1], e.listener[2],
                          e.right[0], e.right[1], e.right[2]);
            f << buf;
        }
        f << "\n], \"marks\": [\n";
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < m.marks.size(); ++i)
            f << (i ? ",\n" : "") << "{\"t\": " << (m.marks[i].first - t0) << ", \"label\": \"" << jsonEscape(m.marks[i].second)
              << "\"}";
        f << "\n]}\n";
    }
    for (auto& p : m.pcm) free(p.data);
    delete impl_;
    impl_ = nullptr;
}

void Engine::play(const PlayRequest& r) {
    {
        std::lock_guard lock(mutex_);
        commands_.push_back({Command::Play, r});
    }
    cv_.notify_one();
}

void Engine::stopHandle(u32 handle) {
    PlayRequest r;
    r.handle = handle;
    std::lock_guard lock(mutex_);
    commands_.push_back({Command::Stop, r});
}

void Engine::stopMatching(int event, int category) {
    PlayRequest r;
    r.event = event;
    r.file = category;
    std::lock_guard lock(mutex_);
    commands_.push_back({Command::StopMatching, r});
}

void Engine::setPosition(u32 handle, const double pos[3]) {
    PlayRequest r;
    r.handle = handle;
    std::copy(pos, pos + 3, r.pos);
    std::lock_guard lock(mutex_);
    commands_.push_back({Command::Position, r});
}

void Engine::setVolume(u32 handle, float volume) {
    PlayRequest r;
    r.handle = handle;
    r.volume = volume;
    std::lock_guard lock(mutex_);
    commands_.push_back({Command::Volume, r});
}

void Engine::setListener(const double pos[3], const float forward[3], const float up[3]) {
    Command c{Command::Listener, {}};
    std::copy(pos, pos + 3, c.req.pos);
    std::copy(forward, forward + 3, c.dir);
    std::copy(up, up + 3, c.dir + 3);
    std::lock_guard lock(mutex_);
    commands_.push_back(c);
}

void Engine::takeFinished(std::vector<u32>& out) {
    std::lock_guard lock(mutex_);
    out.insert(out.end(), finished_.begin(), finished_.end());
    finished_.clear();
}

void Engine::preload(const std::vector<int>& files) {
    std::lock_guard lock(mutex_);
    preloadFiles_.insert(preloadFiles_.end(), files.begin(), files.end());
}

void Engine::mark(const char* label) {
    if (!impl_ || !capturing()) return;
    std::lock_guard lock(mutex_);
    impl_->marks.push_back({nowSeconds(), label});
}

Engine::Stats Engine::stats() const {
    std::lock_guard lock(mutex_);
    return impl_ ? impl_->stats : Stats{};
}

void Engine::run() {
    Impl& m = *impl_;
    std::vector<Command> cmds;
    cmds.reserve(1024);
    std::vector<u32> done;
    done.reserve(512);

    auto loadPcm = [&](int file) -> Impl::Pcm* {
        Impl::Pcm& p = m.pcm[file];
        if (p.data || p.failed) return p.failed ? nullptr : &p;
        Timer t;
        std::vector<u8> bytes;
        int channels = 0, rate = 0;
        short* out = nullptr;
        const int frames = bank_->readBytes(file, bytes)
                               ? stb_vorbis_decode_memory(bytes.data(), int(bytes.size()), &channels, &rate, &out)
                               : -1;
        if (frames <= 0 || !out || channels < 1 || channels > 2) {
            free(out);
            p.failed = true;
            LOG("audio: cannot decode %s", bank_->fileName(file).c_str());
            return nullptr;
        }
        p = {out, u32(frames), channels, rate, false};
        std::lock_guard lock(mutex_);
        m.stats.decoded++;
        m.stats.decodeMs += t.ms();
        return &p;
    };

    // Gain of each output channel for a voice at the current listener.
    auto computeMatrix = [&](const PlayRequest& r, int channels, float out[4]) -> float {
        double rel[3];
        for (int a = 0; a < 3; ++a) rel[a] = r.relative ? r.pos[a] : r.pos[a] - m.listener[a];
        const double dist = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
        float g = r.volume * gains_[int(r.category)].load(std::memory_order_relaxed) *
                  (r.category == Category::Master ? 1.0f : gains_[0].load(std::memory_order_relaxed));
        if (r.category == Category::Music) g *= musicFade_.load(std::memory_order_relaxed);
        if (r.linear) g *= float(std::max(0.0, 1.0 - dist / std::max(1e-3f, r.attenuation)));
        if (channels == 2) {  // multichannel sounds are not spatialised (OpenAL), only attenuated
            out[0] = g, out[1] = 0, out[2] = 0, out[3] = g;
            return g;
        }
        float x = 0;  // -1 left .. 1 right
        if (dist > 1e-4) {
            if (r.relative)
                x = float(rel[0] / dist);  // listener space: +x is right
            else
                x = float((rel[0] * m.right[0] + rel[1] * m.right[1] + rel[2] * m.right[2]) / dist);
        }
        out[0] = g * std::sqrt(0.5f * (1.0f - x));
        out[1] = g * std::sqrt(0.5f * (1.0f + x));
        return g;
    };

    auto applyMatrix = [&](Impl::Voice& v) {
        float mtx[4];
        v.gain = computeMatrix(v.req, v.channels, mtx);
        const int n = v.channels * 2;
        bool changed = false;
        for (int i = 0; i < n; ++i) changed |= std::abs(mtx[i] - v.matrix[i]) > 1e-5f;
        if (!changed) return;
        std::copy(mtx, mtx + n, v.matrix);
        // XAudio2 matrix layout: [destination][source]
        float x[4];
        if (v.channels == 1)
            x[0] = mtx[0], x[1] = mtx[1];
        else
            x[0] = mtx[0], x[1] = mtx[1], x[2] = mtx[2], x[3] = mtx[3];
        v.sv->SetOutputMatrix(nullptr, v.channels, 2, x);
    };

    auto acquire = [&](int channels, int rate) -> IXAudio2SourceVoice* {
        const u32 key = (u32(channels) << 24) | u32(rate);
        auto& list = m.pool[key];
        if (!list.empty()) {
            IXAudio2SourceVoice* sv = list.back();
            list.pop_back();
            return sv;
        }
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = WORD(channels);
        fmt.nSamplesPerSec = DWORD(rate);
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = WORD(channels * 2);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        IXAudio2SourceVoice* sv = nullptr;
        if (FAILED(m.xa->CreateSourceVoice(&sv, &fmt, 0, 2.0f))) return nullptr;
        return sv;
    };

    auto release = [&](Impl::Voice& v) {  // the voice has no queued buffers
        auto& list = m.pool[v.key];
        if (list.size() < 32)
            list.push_back(v.sv);
        else
            v.sv->DestroyVoice();
        v.sv = nullptr;
        v.stream.reset();
    };

    auto beginStop = [&](Impl::Voice& v) {
        if (v.draining) return;
        v.sv->Stop(0);
        v.sv->FlushSourceBuffers();
        v.draining = true;
        done.push_back(v.req.handle);
    };

    // Decodes and queues stream buffers until kStreamBuffers are queued; false at the end.
    auto pump = [&](Impl::Voice& v) {
        Impl::Stream& s = *v.stream;
        XAUDIO2_VOICE_STATE st;
        v.sv->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        u32 queued = st.BuffersQueued;
        const int chunk = int(s.rate * kStreamBufferSeconds);
        while (queued < kStreamBuffers && !s.eof) {
            std::vector<short>& buf = s.bufs[s.next];
            buf.resize(size_t(chunk) * s.channels);
            int got = 0;
            bool looped = false;
            while (got < chunk) {
                const int n = stb_vorbis_get_samples_short_interleaved(s.vf, s.channels, buf.data() + size_t(got) * s.channels,
                                                                       (chunk - got) * s.channels);
                if (n > 0) {
                    got += n;
                    looped = false;
                    continue;
                }
                if (s.loop && !looped && stb_vorbis_seek_start(s.vf)) {
                    looped = true;
                    continue;
                }
                s.eof = true;
                break;
            }
            if (got == 0 && s.eof) {  // nothing left: mark the end with an empty-ish buffer
                if (queued == 0) return false;
                break;
            }
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = UINT32(size_t(got) * s.channels * sizeof(short));
            xb.pAudioData = reinterpret_cast<const BYTE*>(buf.data());
            if (s.eof) xb.Flags = XAUDIO2_END_OF_STREAM;
            v.sv->SubmitSourceBuffer(&xb);
            s.next = (s.next + 1) % kStreamBuffers;
            ++queued;
        }
        return true;
    };

    auto startVoice = [&](const PlayRequest& r) {
        int streaming = 0, statics = 0;
        for (const auto& v : m.voices) (v.req.stream ? streaming : statics)++;
        std::unique_ptr<Impl::Stream> stream;
        Impl::Pcm* pcm = nullptr;
        int channels, rate;
        if (r.stream) {
            if (streaming >= kStreamingVoices) {
                std::lock_guard lock(mutex_);
                m.stats.dropped++;
                return false;
            }
            stream = std::make_unique<Impl::Stream>();
            int err = 0;
            if (bank_->readBytes(r.file, stream->file))
                stream->vf = stb_vorbis_open_memory(stream->file.data(), int(stream->file.size()), &err, nullptr);
            if (!stream->vf) {
                LOG("audio: cannot open stream %s", bank_->fileName(r.file).c_str());
                return false;
            }
            const stb_vorbis_info info = stb_vorbis_get_info(stream->vf);
            stream->channels = info.channels, stream->rate = int(info.sample_rate), stream->loop = r.looping;
            if (info.channels < 1 || info.channels > 2) return false;
            channels = stream->channels, rate = stream->rate;
        } else {
            pcm = loadPcm(r.file);
            if (!pcm) return false;
            channels = pcm->channels, rate = pcm->rate;
        }
        if (!r.stream && statics >= kStaticVoices) {
            // Full: replace the quietest (then oldest) voice if the new sound is louder.
            float tmp[4];
            const float g = computeMatrix(r, channels, tmp);
            Impl::Voice* victim = nullptr;
            for (auto& v : m.voices)
                if (!v.req.stream && !v.draining &&
                    (!victim || v.gain < victim->gain || (v.gain == victim->gain && v.started < victim->started)))
                    victim = &v;
            std::lock_guard lock(mutex_);
            if (!victim || victim->gain > g) {
                m.stats.dropped++;
                return false;
            }
            m.stats.stolen++;
            victim->sv->Stop(0);
            victim->sv->FlushSourceBuffers();
            victim->draining = true;
            finished_.push_back(victim->req.handle);
        }
        IXAudio2SourceVoice* sv = acquire(channels, rate);
        if (!sv) return false;
        m.voices.emplace_back();
        Impl::Voice& v = m.voices.back();
        v.sv = sv;
        v.key = (u32(channels) << 24) | u32(rate);
        v.channels = channels;
        v.req = r;
        v.stream = std::move(stream);
        v.started = nowSeconds();
        sv->SetFrequencyRatio(r.pitch);
        v.pitchSet = r.pitch;
        applyMatrix(v);
        if (v.stream) {
            if (!pump(v)) {
                v.draining = true;
                return false;
            }
        } else {
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = pcm->frames * u32(pcm->channels) * 2;
            xb.pAudioData = reinterpret_cast<const BYTE*>(pcm->data);
            xb.Flags = XAUDIO2_END_OF_STREAM;
            if (r.looping) xb.LoopCount = XAUDIO2_LOOP_INFINITE;
            sv->SubmitSourceBuffer(&xb);
        }
        sv->Start(0);
        if (m.capture) {
            Impl::LogEntry e{r.requestTime, v.started, r.event, r.file, r, {v.matrix[0], v.matrix[1]}, {}, {}};
            std::copy(m.right, m.right + 3, e.right);
            if (channels == 2) e.gain[1] = v.matrix[3];
            std::copy(m.listener, m.listener + 3, e.listener);
            m.log.push_back(e);
        }
        return true;
    };

    for (;;) {
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(5), [&] { return quit_ || !commands_.empty(); });
            if (quit_) break;
            cmds.swap(commands_);
            for (int f : preloadFiles_) m.preloadQueue.push_back(f);
            preloadFiles_.clear();
        }
        for (const Command& c : cmds) {
            switch (c.type) {
            case Command::Play:
                if (!startVoice(c.req)) done.push_back(c.req.handle);
                break;
            case Command::Stop:
                for (auto& v : m.voices)
                    if (v.req.handle == c.req.handle) beginStop(v);
                break;
            case Command::StopMatching:
                for (auto& v : m.voices)
                    if ((c.req.event < 0 || v.req.event == c.req.event) && (c.req.file < 0 || int(v.req.category) == c.req.file))
                        beginStop(v);
                break;
            case Command::Position:
                for (auto& v : m.voices)
                    if (v.req.handle == c.req.handle) std::copy(c.req.pos, c.req.pos + 3, v.req.pos);
                break;
            case Command::Volume:
                for (auto& v : m.voices)
                    if (v.req.handle == c.req.handle) v.req.volume = std::clamp(c.req.volume, 0.0f, 1.0f);
                break;
            case Command::Listener: {
                std::copy(c.req.pos, c.req.pos + 3, m.listener);
                std::copy(c.dir, c.dir + 3, m.forward);
                std::copy(c.dir + 3, c.dir + 6, m.up);
                // right = forward x up
                m.right[0] = m.forward[1] * m.up[2] - m.forward[2] * m.up[1];
                m.right[1] = m.forward[2] * m.up[0] - m.forward[0] * m.up[2];
                m.right[2] = m.forward[0] * m.up[1] - m.forward[1] * m.up[0];
                break;
            }
            default:
                break;
            }
        }
        cmds.clear();

        // Service voices: streams, finished sounds, gains.
        for (size_t i = 0; i < m.voices.size();) {
            Impl::Voice& v = m.voices[i];
            XAUDIO2_VOICE_STATE st;
            v.sv->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (v.draining) {
                if (st.BuffersQueued == 0) {
                    release(v);
                    m.voices[i] = std::move(m.voices.back());
                    m.voices.pop_back();
                    continue;
                }
            } else if (v.stream ? (v.stream->eof && st.BuffersQueued == 0) : st.BuffersQueued == 0) {
                v.sv->Stop(0);
                done.push_back(v.req.handle);
                release(v);
                m.voices[i] = std::move(m.voices.back());
                m.voices.pop_back();
                continue;
            } else {
                if (v.stream) pump(v);
                applyMatrix(v);
            }
            ++i;
        }
        if (!m.preloadQueue.empty()) {  // one preload per pass keeps the thread responsive
            loadPcm(m.preloadQueue.front());
            m.preloadQueue.pop_front();
        }
        {
            std::lock_guard lock(mutex_);
            finished_.insert(finished_.end(), done.begin(), done.end());
            m.stats.voices = u32(m.voices.size());
            m.stats.streams = 0;
            for (const auto& v : m.voices) m.stats.streams += v.req.stream;
        }
        done.clear();
    }
}

}  // namespace audio
