#include "voice_internal.h"

#include <sapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace vb::voice {
namespace {

constexpr Time NoDeadline = Time::max();
constexpr DWORD SpeakFlags = SPF_ASYNC | SPF_IS_NOT_XML;
constexpr wchar_t NarratorKey[] = L"Software\\Microsoft\\Narrator";
constexpr wchar_t NarratorNoRoamKey[] = L"Software\\Microsoft\\Narrator\\NoRoam";

int ReadSpeed(const wchar_t* name) {
    DWORD speed = 0;
    DWORD bytes = sizeof(speed);
    if (RegGetValueW(HKEY_CURRENT_USER, NarratorKey, name, RRF_RT_REG_DWORD,
                     nullptr, &speed, &bytes) != ERROR_SUCCESS || bytes != sizeof(speed)) return -1;
    return static_cast<int>(std::min(speed, 20UL));
}

Settings ReadSettings() {
#ifdef VB_VOICE_TEST
    if (test::narratorSource) {
        auto snapshot = test::narratorSource->read();
        return {std::move(snapshot.voice), snapshot.neuralSpeed, snapshot.speechSpeed};
    }
#endif
    Settings settings;
    wchar_t name[1024] = {};
    DWORD bytes = sizeof(name);
    if (RegGetValueW(HKEY_CURRENT_USER, NarratorNoRoamKey,
                     L"SpeechVoice", RRF_RT_REG_SZ, nullptr, name, &bytes) == ERROR_SUCCESS) {
        settings.narratorVoice = name;
    }
    settings.neuralSpeed = ReadSpeed(L"NeuralSpeechSpeed");
    settings.speechSpeed = ReadSpeed(L"SpeechSpeed");
    return settings;
}

class NarratorWatch {
public:
    ~NarratorWatch() { if (key_) RegCloseKey(key_); }

    bool arm() {
#ifdef VB_VOICE_TEST
        if (test::narratorSource) return test::narratorSource->event != nullptr;
#endif
        if (!changed_.get()) return false;
        // The persistent scheduler thread owns the one-shot registration.
        if (!key_ && RegOpenKeyExW(HKEY_CURRENT_USER, NarratorKey, 0, KEY_NOTIFY, &key_) != ERROR_SUCCESS) return false;
        if (RegNotifyChangeKeyValue(key_, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME,
                                    changed_.get(), TRUE) == ERROR_SUCCESS) return true;
        RegCloseKey(std::exchange(key_, nullptr));
        ResetEvent(changed_.get());  // Closing the failed key can signal the event.
        return false;
    }

    HANDLE event() const {
#ifdef VB_VOICE_TEST
        if (test::narratorSource) return test::narratorSource->event;
#endif
        return changed_.get();
    }

private:
    Event changed_;
    HKEY key_ = nullptr;
};

std::wstring Normalize(const std::wstring& name) {
    std::wstring normalized;
    for (wchar_t ch : name) {
        WORD type = 0;
        if (GetStringTypeW(CT_CTYPE1, &ch, 1, &type) && !(type & (C1_SPACE | C1_PUNCT))) {
            normalized.push_back(ch);
        }
    }
    if (!normalized.empty()) {
        CharLowerBuffW(normalized.data(), static_cast<DWORD>(normalized.size()));
    }
    return normalized;
}

std::wstring Identifier(ISpObjectToken& token) {
    wchar_t* value = nullptr;
    token.GetId(&value);
    std::wstring result(value ? value : L"");
    CoTaskMemFree(value);
    return result;
}

std::wstring Description(ISpObjectToken& token) {
    wchar_t* value = nullptr;
    wchar_t language[12] = {};
    swprintf(language, std::size(language), L"%x", GetUserDefaultUILanguage());
    if (FAILED(token.GetStringValue(language, &value))) token.GetStringValue(nullptr, &value);
    std::wstring result(value ? value : L"");
    CoTaskMemFree(value);
    return result;
}

struct Selection {
    ComPtr<ISpObjectToken> token;
    Choice choice;
};

HRESULT SelectVoices(const SpeakerConfig& config, const Settings& settings,
                     const std::vector<std::wstring>& excluded, std::vector<Selection>& choices) {
    ComPtr<ISpObjectTokenCategory> category;
    HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ISpObjectTokenCategory, category.voidOut());
    if (SUCCEEDED(hr)) hr = category->SetId(SPCAT_VOICES, FALSE);
    if (FAILED(hr)) return hr;

    wchar_t* defaultId = nullptr;
    category->GetDefaultTokenId(&defaultId);
    const std::wstring defaultKey = defaultId ? defaultId : L"";
    CoTaskMemFree(defaultId);

    const auto narrator = config.followNarrator ? Normalize(settings.narratorVoice) : std::wstring{};
    const auto wanted = Normalize(config.voice);
    const auto fallback = Normalize(config.fallbackVoice);
    bool foundDefault = false;
    const auto add = [&](ComPtr<ISpObjectToken> token) {
        auto id = Identifier(*token.get());
        if (id.empty()) return;
        const bool isDefault = !defaultKey.empty() && _wcsicmp(id.c_str(), defaultKey.c_str()) == 0;
        foundDefault = foundDefault || isDefault;
        auto name = Description(*token.get());
        const auto normalized = Normalize(name);
        int rank = 4;
        if (!narrator.empty() && normalized == narrator) rank = 0;
        else if (!wanted.empty() && normalized.find(wanted) != std::wstring::npos) rank = 1;
        else if (!fallback.empty() && normalized.find(fallback) != std::wstring::npos) rank = 2;
        else if (isDefault) rank = 3;
        const bool failed = std::any_of(excluded.begin(), excluded.end(), [&](const auto& prior) {
            return _wcsicmp(id.c_str(), prior.c_str()) == 0;
        });
        const int priority = failed ? 5 : rank;
        const bool natural = normalized.find(L"natural") != std::wstring::npos;
        const int speed = natural ? settings.neuralSpeed : settings.speechSpeed;
        const int rate = std::clamp(config.followNarrator && speed >= 0 ? speed - 10 : config.rate, -10, 10);
        choices.push_back({std::move(token), {std::move(id), std::move(name), {}, rate, rank >= 2, priority}});
    };
    ComPtr<IEnumSpObjectTokens> tokens;
    hr = category->EnumTokens(nullptr, nullptr, tokens.out());
    while (SUCCEEDED(hr) && tokens) {
        ComPtr<ISpObjectToken> token;
        hr = tokens->Next(1, token.out(), nullptr);
        if (hr != S_OK || !token) break;
        add(std::move(token));
    }
    if (!foundDefault && !defaultKey.empty()) {
        ComPtr<ISpObjectToken> token;
        if (SUCCEEDED(CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_ISpObjectToken, token.voidOut())) &&
            SUCCEEDED(token->SetId(SPCAT_VOICES, defaultKey.c_str(), FALSE))) {
            add(std::move(token));
        }
    }
    if (choices.empty()) return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    std::stable_sort(choices.begin(), choices.end(), [](const Selection& left, const Selection& right) {
        return left.choice.priority < right.choice.priority;
    });
    return S_OK;
}

// MinGW does not ship sapiddk.h or define SPDFID_WaveFormatEx in its SAPI
// import library. Use the SDK's ISpTTSEngine COM vtable to query the token.
constexpr GUID WaveFormatId = {0xc31adbae, 0x527f, 0x4ff5, {0xa2, 0x30, 0xf6, 0x2b, 0xb6, 0x1f, 0xf7, 0x0c}};
constexpr IID TtsEngineId = {0xa74d7c8e, 0x4cc5, 0x4f2f, {0xa6, 0xeb, 0x80, 0x4d, 0xee, 0x18, 0x50, 0x0e}};

struct TtsEngine;
struct TextFragment;
struct TtsEngineSite;
struct TtsVtable {
    HRESULT (STDMETHODCALLTYPE *queryInterface)(TtsEngine*, REFIID, void**);
    ULONG (STDMETHODCALLTYPE *addRef)(TtsEngine*);
    ULONG (STDMETHODCALLTYPE *release)(TtsEngine*);
    HRESULT (STDMETHODCALLTYPE *speak)(TtsEngine*, DWORD, REFGUID, const WAVEFORMATEX*,
                                      const TextFragment*, TtsEngineSite*);
    HRESULT (STDMETHODCALLTYPE *getOutputFormat)(TtsEngine*, const GUID*, const WAVEFORMATEX*,
                                                 GUID*, WAVEFORMATEX**);
};

struct TtsEngine {
    const TtsVtable* table;
    ULONG Release() { return table->release(this); }
};

std::optional<Format> NativeFormat(ISpObjectToken& token) {
    ComPtr<TtsEngine> engine;
    HRESULT hr = token.CreateInstance(nullptr, CLSCTX_INPROC_SERVER, TtsEngineId, engine.voidOut());
    if (FAILED(hr) || !engine) return {};
    WAVEFORMATEX* wave = nullptr;
    GUID id{};
    // A null target asks ISpTTSEngine for its default format.
    hr = engine->table->getOutputFormat(engine.get(), nullptr, nullptr, &id, &wave);
    std::optional<Format> result;
    // The output uses one or two channels in plain WAVEFORMATEX. Millisecond
    // frame calculations and the audio buffer also need a usable sample rate.
    if (SUCCEEDED(hr) && wave && id == WaveFormatId && wave->wFormatTag == WAVE_FORMAT_PCM &&
        wave->wBitsPerSample == 16 && wave->nChannels >= 1 && wave->nChannels <= 2 &&
        wave->nSamplesPerSec >= 1000 && wave->nSamplesPerSec <= 384000 &&
        wave->nBlockAlign == wave->nChannels * sizeof(Sample) &&
        wave->nAvgBytesPerSec == wave->nSamplesPerSec * wave->nBlockAlign) {
        result = Format{wave->nSamplesPerSec, wave->nChannels};
    }
    CoTaskMemFree(wave);
    return result;
}

struct CacheKey {
    std::wstring_view voice;
    int rate;
    std::wstring_view text;
    bool operator<(const CacheKey& other) const {
        if (rate != other.rate) return rate < other.rate;
        if (voice != other.voice) return voice < other.voice;
        return text < other.text;
    }
};

struct CacheBudget {
    explicit CacheBudget(size_t limit) : limit(limit) {}
    const size_t limit;
    std::atomic<size_t> used{0};

    void charge(size_t bytes) { report(used.fetch_add(bytes) + bytes); }
    void release(size_t bytes) { report(used.fetch_sub(bytes) - bytes); }

    static void report([[maybe_unused]] size_t bytes) {
#ifdef VB_VOICE_TEST
        test::Observe({test::Kind::CacheBytes, 0, 0, static_cast<double>(bytes)});
#endif
    }
};

struct CacheCharge {
    CacheCharge(std::shared_ptr<CacheBudget> budget, size_t bytes = 0)
        : budget(std::move(budget)), bytes(bytes) {}
    CacheCharge(CacheCharge&&) = default;
    ~CacheCharge() { if (budget && bytes) budget->release(bytes); }

    void swap(CacheCharge& other) { std::swap(bytes, other.bytes); }
    std::shared_ptr<CacheBudget> budget;
    size_t bytes;
};

struct CachePcm {
    CachePcm(CacheCharge metadata, const Choice& choice, const Job& job)
        : metadata(std::move(metadata)), storage(this->metadata.budget),
#ifdef VB_VOICE_TEST
          jobId(job.id),
#endif
          voice(choice.id), text(job.text), rate(choice.rate), format(choice.format) {}

    CacheKey key() const { return {voice, rate, text}; }
    CacheCharge metadata;
    CacheCharge storage;
#ifdef VB_VOICE_TEST
    const std::uint64_t jobId;
#endif
    const std::wstring voice;
    const std::wstring text;
    const int rate;
    const Format format;
    Samples samples;
};

class PhraseCache {
public:
    explicit PhraseCache(int megabytes)
        : budget_(std::make_shared<CacheBudget>(static_cast<size_t>(std::max(0, megabytes)) * 1024 * 1024)) {}

    std::shared_ptr<CachePcm> begin(const Choice& choice, const Job& job) {
        if (!budget_->limit) return {};
        const size_t bytes = sizeof(CachePcm) + sizeof(CacheKey) + 256 +
                             (choice.id.size() + job.text.size() + 2) * sizeof(wchar_t);
        if (!reserve(bytes)) return {};
        try {
            return std::make_shared<CachePcm>(CacheCharge(budget_, bytes), choice, job);
        } catch (const std::bad_alloc&) {
            return {};
        }
    }

    bool append(CachePcm& pcm, const Samples& samples, size_t limit) {
        const size_t total = pcm.samples.size() + samples.size();
        if (total >= limit) return false;
        if (total > pcm.samples.capacity()) {
            const auto capacity = std::min(limit, std::max(total, pcm.samples.capacity() * 2));
            if (!reserve(capacity * sizeof(Sample))) return false;
            CacheCharge charge(budget_, capacity * sizeof(Sample));
            try {
                Samples grown;
                grown.reserve(capacity);
                if (grown.capacity() > capacity) {
                    const size_t extra = (grown.capacity() - capacity) * sizeof(Sample);
                    if (!reserve(extra)) return false;
                    charge.bytes += extra;
                }
                grown.insert(grown.end(), pcm.samples.begin(), pcm.samples.end());
                grown.insert(grown.end(), samples.begin(), samples.end());
                pcm.samples.swap(grown);
                pcm.storage.swap(charge);
            } catch (const std::bad_alloc&) {
                return false;
            }
        } else {
            pcm.samples.insert(pcm.samples.end(), samples.begin(), samples.end());
        }
        return true;
    }

    std::shared_ptr<const Samples> find(const Choice& choice, const Job& job) {
        if (!budget_->limit) return {};
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = index_.find({choice.id, choice.rate, job.text});
        if (found == index_.end() || (*found->second)->format != choice.format) return {};
        entries_.splice(entries_.begin(), entries_, found->second);
        const auto& pcm = *found->second;
        return {pcm, &pcm->samples};
    }

    void insert(std::shared_ptr<CachePcm> pcm) {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = index_.find(pcm->key());
        if (found != index_.end()) {
            if ((*found->second)->format == pcm->format) {
                entries_.splice(entries_.begin(), entries_, found->second);
                return;
            }
            const auto entry = found->second;
            index_.erase(found);
            entries_.erase(entry);
        }
        try {
            entries_.push_front(std::move(pcm));
            try {
                index_.emplace(entries_.front()->key(), entries_.begin());
            } catch (const std::bad_alloc&) {
                entries_.pop_front();
            }
        } catch (const std::bad_alloc&) {
            // Allocation failure disables caching for this phrase only.
        }
    }

    void clear() {
        const std::lock_guard<std::mutex> lock(mutex_);
        index_.clear();
        entries_.clear();
    }

private:
    bool reserve(size_t bytes) {
        if (bytes > budget_->limit) return false;
        const std::lock_guard<std::mutex> lock(mutex_);
        // A hit may hold its PCM in the playback queue. Skip collecting
        // instead of waiting for that allocation or evicting newer entries.
        while (bytes > budget_->limit - budget_->used.load()) {
            if (entries_.empty() || entries_.back().use_count() != 1) return false;
#ifdef VB_VOICE_TEST
            test::Observe({test::Kind::CacheEvicted, entries_.back()->jobId});
#endif
            index_.erase(entries_.back()->key());
            entries_.pop_back();
        }
        budget_->charge(bytes);
        return true;
    }

    std::shared_ptr<CacheBudget> budget_;
    std::mutex mutex_;
    std::list<std::shared_ptr<CachePcm>> entries_;
    std::map<CacheKey, decltype(entries_)::iterator> index_;
};

struct Slot {
    explicit Slot(std::shared_ptr<PhraseCache> phraseCache) : cache(std::move(phraseCache)) {}
    const std::shared_ptr<PhraseCache> cache;
    Event wake;
    std::atomic<bool> abandoned{false};
    std::atomic<bool> exited{false};
    std::vector<std::wstring> excluded;  // Fixed at worker creation.
    unsigned excludedRevision = 0;
    std::atomic<Time> callAt{NoDeadline};
    std::atomic<Time> firstDeadline{NoDeadline};
    std::atomic<Time> progressDeadline{NoDeadline};

    // The remaining fields are protected by State::mutex.
    Settings settings;
    unsigned revision = 0;
    unsigned readyRevision = 0;
    Choice choice;
    std::wstring attemptedId;
    std::shared_ptr<Job> job;
    bool done = false;
    bool probePreferred = false;
    bool purgeRequested = false;
    bool failed = false;
    HRESULT error = S_OK;
    std::shared_ptr<CachePcm> complete;
};

class CallTimer {
public:
    CallTimer(State& state, Slot& slot) : state_(state), slot_(slot) {
        slot_.callAt = Clock::now();
        state_.dispatch.signal();
    }
    ~CallTimer() {
        slot_.callAt = NoDeadline;
        state_.dispatch.signal();
    }
private:
    State& state_;
    Slot& slot_;
};

bool Live(const State& state, const Slot& slot, const std::shared_ptr<Job>& job) {
    return !state.stop && !slot.abandoned && (!job || job->generation == state.generation);
}

#ifdef VB_VOICE_TEST
std::mutex stallMutex;
std::condition_variable stallChanged;
unsigned stallCount = 0;
unsigned stallEpoch = 0;

void InjectStall(const State& state, bool forced = false) {
    std::unique_lock<std::mutex> lock(stallMutex);
    if (state.stop || (!forced && !stallCount)) return;
    if (!forced) --stallCount;
    const unsigned epoch = stallEpoch;
    test::Observe({test::Kind::StallEntered});
    stallChanged.wait(lock, [&] { return epoch != stallEpoch; });
}
#endif

class Capture {
public:
    Capture(const std::shared_ptr<State>& state, const std::shared_ptr<Slot>& slot,
            std::shared_ptr<Job> job, const Choice& choice)
        : stateOwner_(state), slotOwner_(slot), state_(*state), slot_(*slot),
          job_(std::move(job)), format_(choice.format),
#ifdef VB_VOICE_TEST
          silentWarm_(!job_ && test::Flag("VOICEBRIDGE_TEST_SILENT_WARM") &&
                      Normalize(choice.name).find(L"jenny") != std::wstring::npos),
#endif
          cached_(job_ ? slot_.cache->begin(choice, *job_) : nullptr) {}

    bool write(const Sample* samples, size_t count) {
        if (!Live(state_, slot_, job_)) return false;
        if (!count) return true;
        received_ = true;
        slot_.progressDeadline = Clock::now() + 10s;
        const size_t channels = format_.channels;
        const size_t lead = format_.frames(15) * channels;
        const size_t fade = format_.frames(3) * channels;
        if (!started_) {
            auto loud = std::find_if(samples, samples + count, [](Sample value) { return std::abs(value) > Loud; });
#ifdef VB_VOICE_TEST
            if (silentWarm_) loud = samples + count;
#endif
            if (loud == samples + count) {
                if (job_) retainLead(samples, count, lead);
                return true;
            }
            started_ = true;
            slot_.firstDeadline = NoDeadline;
            state_.dispatch.signal();
            if (!job_) return true;
            const size_t prefix = static_cast<size_t>(loud - samples) / channels * channels;
            retainLead(samples, prefix, lead);
            auto trimmed = std::move(lead_);
            trimmed.insert(trimmed.end(), samples + prefix, samples + count);
#ifdef VB_VOICE_TEST
            test::Observe({test::Kind::RawOnset, job_->id, job_->generation,
                           static_cast<double>((trimmed.size() - (count - prefix)) / channels), trimmed.data(), trimmed.size(),
                           format_.rate, format_.channels});
#endif
            return publish(std::move(trimmed), fade);
        }
        return !job_ || publish(Samples(samples, samples + count), fade);
    }

    bool received() const { return received_; }
    bool audible() const { return started_; }

    std::shared_ptr<CachePcm> finish() {
        if (!job_) return {};
        const size_t keep = format_.frames(80);
        size_t toTrim = trailing_ > keep ? trailing_ - keep : 0;
        const size_t cacheTrim = toTrim;
        {
            std::lock_guard<std::mutex> lock(state_.mutex);
            if (!Live(state_, slot_, job_)) return {};
            while (toTrim && !state_.audio.empty()) {
                auto& chunk = state_.audio.back();
                if (chunk.id != job_->id) break;
                const size_t frames = std::min(toTrim, chunk.count);
                chunk.count -= frames;
                job_->bufferedFrames -= frames;
                toTrim -= frames;
                state_.waiting = std::max(0.0, state_.waiting - format_.seconds(frames));
                if (!chunk.count) state_.audio.pop_back();
            }
        }
        const size_t trim = cacheTrim * format_.channels;
        if (!cached_ || cached_->samples.size() <= trim) return {};
        const size_t length = cached_->samples.size() - trim;
        if (length >= format_.rate * format_.channels * 10) return {};
        cached_->samples.resize(length);
        return std::move(cached_);
    }

private:
    void retainLead(const Sample* samples, size_t count, size_t lead) {
        if (count >= lead) {
            lead_.assign(samples + count - lead, samples + count);
        } else {
            if (lead_.size() + count > lead) {
                const auto discard = lead_.size() + count - lead;
                lead_.erase(lead_.begin(), lead_.begin() + discard);
            }
            lead_.insert(lead_.end(), samples, samples + count);
        }
    }

    bool publish(Samples pcm, size_t fade) {
        if (pcm.empty()) return true;
        const size_t channels = format_.channels;
        auto lastLoud = std::find_if(pcm.rbegin(), pcm.rend(), [](Sample value) { return std::abs(value) > Loud; });
        if (lastLoud == pcm.rend()) trailing_ += pcm.size() / channels;
        else trailing_ = static_cast<size_t>(lastLoud - pcm.rbegin()) / channels;

        for (size_t i = 0; i < pcm.size() && faded_ < fade; ++i, ++faded_) {
            pcm[i] = static_cast<Sample>(pcm[i] * static_cast<int>(faded_ / channels) /
                                         static_cast<int>(fade / channels));
        }
        if (cached_) {
            // A candidate may temporarily include trailing silence, which is
            // removed before checking the ten-second cache limit.
            if (!slot_.cache->append(*cached_, pcm, format_.rate * channels * 11)) cached_.reset();
        }
        auto block = std::make_shared<const Samples>(std::move(pcm));
        size_t offset = 0;
        const size_t total = block->size() / channels;
        std::unique_lock<std::mutex> lock(state_.mutex);
        while (offset < total) {
            state_.space.wait(lock, [&] {
                return !Live(state_, slot_, job_) || state_.waiting + 1.0 / format_.rate <= BufferSeconds;
            });
            if (!Live(state_, slot_, job_)) return false;
            const size_t available = static_cast<size_t>((BufferSeconds - state_.waiting) * format_.rate + 1e-6);
            const size_t count = std::min(total - offset, std::max<size_t>(1, available));
            slot_.progressDeadline = Clock::now() + 10s;
            state_.queueChunk({block, offset, count, format_, job_->id, job_->generation, job_});
            offset += count;
        }
        return true;
    }

    // SAPI can still own the output stream after its worker has been retired.
    std::shared_ptr<State> stateOwner_;
    std::shared_ptr<Slot> slotOwner_;
    State& state_;
    Slot& slot_;
    std::shared_ptr<Job> job_;
    Format format_;
    Samples lead_;
#ifdef VB_VOICE_TEST
    bool silentWarm_;
#endif
    std::shared_ptr<CachePcm> cached_;
    size_t faded_ = 0;
    size_t trailing_ = 0;
    bool started_ = false;
    bool received_ = false;
};

class OutputStream final : public IStream {
public:
    OutputStream(std::shared_ptr<Capture> capture, unsigned channels)
        : capture_(std::move(capture)), channels_(channels) {}
    ~OutputStream() = default;
    HRESULT marshal() { return CoCreateFreeThreadedMarshaler(this, marshaler_.out()); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_ISequentialStream || iid == IID_IStream) {
            *result = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        if (iid == IID_IMarshal && marshaler_) return marshaler_->QueryInterface(iid, result);
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = --references_;
        if (!count) delete this;
        return count;
    }

    void seal() {
        std::lock_guard<std::mutex> lock(mutex_);
        capture_.reset();
    }

    HRESULT STDMETHODCALLTYPE Write(const void* data, ULONG bytes, ULONG* written) override {
        if (written) *written = 0;
        if (!bytes) return S_OK;
        if (!data) return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        if (capture_) {
            try {
                const size_t align = sizeof(Sample) * channels_;
                const auto* first = static_cast<const unsigned char*>(data);
                if (pending_.empty() && bytes % align == 0 &&
                    reinterpret_cast<std::uintptr_t>(data) % alignof(Sample) == 0) {
                    capture_->write(static_cast<const Sample*>(data), bytes / sizeof(Sample));
                } else {
                    pending_.insert(pending_.end(), first, first + bytes);
                    const size_t valid = pending_.size() / align * align;
                    Samples pcm(valid / sizeof(Sample));
                    if (valid) std::memcpy(pcm.data(), pending_.data(), valid);
                    pending_.erase(pending_.begin(), pending_.begin() + valid);
                    capture_->write(pcm.data(), pcm.size());
                }
            } catch (...) {
                return E_OUTOFMEMORY;
            }
        }
        position_ += bytes;
        length_ = std::max(length_, position_);
        if (written) *written = bytes;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Read(void*, ULONG, ULONG* read) override {
        if (read) *read = 0;
        return S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (origin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
        auto base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? position_ : length_;
        if (move.QuadPart < -base || move.QuadPart > std::numeric_limits<LONGLONG>::max() - base) {
            return STG_E_INVALIDFUNCTION;
        }
        position_ = base + move.QuadPart;
        if (result) result->QuadPart = position_;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size.QuadPart > static_cast<ULONGLONG>(std::numeric_limits<LONGLONG>::max())) return STG_E_INVALIDFUNCTION;
        length_ = static_cast<LONGLONG>(size.QuadPart);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
        if (!stat) return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        *stat = {};
        stat->type = STGTY_STREAM;
        stat->grfMode = STGM_WRITE;
        stat->cbSize.QuadPart = length_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    std::atomic<ULONG> references_{1};
    std::mutex mutex_;
    std::shared_ptr<Capture> capture_;
    std::vector<unsigned char> pending_;
    const unsigned channels_;
    LONGLONG position_ = 0;
    LONGLONG length_ = 0;
    ComPtr<IUnknown> marshaler_;
};

class Engine {
public:
    Engine(const std::shared_ptr<State>& state, const std::shared_ptr<Slot>& slot)
        : stateOwner_(state), slotOwner_(slot), state_(*state), slot_(*slot) {}
    HRESULT configure(const Settings& settings, const std::vector<std::wstring>& excluded) {
#ifdef VB_VOICE_TEST
        const bool badJenny = test::Flag("VOICEBRIDGE_TEST_BAD_JENNY");
#endif
        {
            std::lock_guard<std::mutex> lock(state_.mutex);
            slot_.attemptedId.clear();
        }
        std::vector<Selection> choices;
        HRESULT result;
        {
            CallTimer timer(state_, slot_);
#ifdef VB_VOICE_TEST
            static std::atomic<bool> stalledSetup{false};
            if (test::Flag("VOICEBRIDGE_TEST_CALL_STALL") && !stalledSetup.exchange(true))
                InjectStall(state_, true);
#endif
            result = SelectVoices(state_.config, settings, excluded, choices);
        }
        bool rejected = false;
        for (auto& selected : choices) {
            {
                std::lock_guard<std::mutex> lock(state_.mutex);
                if (!Live(state_, slot_, {})) {
                    result = S_FALSE;
                    break;
                }
                slot_.attemptedId = selected.choice.id;
            }
            const bool warm = !voice_ || _wcsicmp(selected.choice.id.c_str(), choice_.id.c_str()) != 0;
            bool fallbackFormat = false;
            {
                CallTimer timer(state_, slot_);
                result = S_OK;
                if (warm) {
#ifdef VB_VOICE_TEST
                    if (badJenny && Normalize(selected.choice.name).find(L"jenny") != std::wstring::npos)
                        result = E_FAIL;
                    else
#endif
                    {
                        if (auto native = NativeFormat(*selected.token.get())) selected.choice.format = *native;
                        else fallbackFormat = true;
                        result = open(selected);
                    }
                } else {
                    selected.choice.format = choice_.format;
                }
                if (SUCCEEDED(result)) result = voice_->SetRate(selected.choice.rate);
                if (SUCCEEDED(result)) {
                    selected.choice.fallback = selected.choice.fallback || rejected;
                    choice_ = std::move(selected.choice);
                }
            }
            if (SUCCEEDED(result) && warm) {
                std::shared_ptr<CachePcm> discard;
                result = speak({}, discard);
                slot_.firstDeadline = NoDeadline;
                slot_.progressDeadline = NoDeadline;
            }
            if (SUCCEEDED(result)) {
                if (result == S_OK && fallbackFormat) {
                    state_.report(LogLevel::Info,
                        L"Using SAPI's 22,050 Hz mono PCM conversion for this voice.", L"native-format");
                }
#ifdef VB_VOICE_TEST
                if (result == S_OK && warm) test::Observe({test::Kind::Warmed});
#endif
                break;
            }
            const auto code = ErrorCode(result);
            const auto key = L"sapi-" + code;
            state_.report(LogLevel::Error, L"SAPI failed (" + code + L").", key.c_str());
            rejected = true;
            {
                CallTimer timer(state_, slot_);
                voice_.reset();
            }
        }
        {
            CallTimer timer(state_, slot_);
            choices.clear();
        }
        return result;
    }

    const Choice& choice() const { return choice_; }

    HRESULT speak(const std::shared_ptr<Job>& job, std::shared_ptr<CachePcm>& complete) {
        if (!Live(state_, slot_, job)) return S_FALSE;
        auto capture = std::make_shared<Capture>(stateOwner_, slotOwner_, job, choice_);
        HANDLE finished;
        HRESULT hr;
        auto output = makeOutput(capture, choice_.format);
        {
            CallTimer timer(state_, slot_);
            hr = attach(*output, choice_.format);
            if (FAILED(hr)) return hr;
            if (!Live(state_, slot_, job)) return S_FALSE;
#ifdef VB_VOICE_TEST
            if (job) {
                static std::atomic<bool> hung{false};
                const bool oldTest = test::Flag("VOICEBRIDGE_TEST_HANG") && !hung.exchange(true);
                InjectStall(state_, oldTest);
                if (!Live(state_, slot_, job)) return S_FALSE;
                if (test::Flag("VOICEBRIDGE_TEST_FAIL_JENNY") &&
                    Normalize(choice_.name).find(L"jenny") != std::wstring::npos) return E_FAIL;
            }
#endif
            finished = voice_->SpeakCompleteEvent();
            if (!finished) return E_FAIL;
            if (!job) {
                slot_.firstDeadline = Clock::now() + 2s;
                state_.dispatch.signal();
            }
            hr = voice_->Speak(job ? job->text.c_str() : L"Ready.", SpeakFlags, nullptr);
        }
        bool purged = false;
        bool finishedSpeaking = false;
        while (SUCCEEDED(hr) && !state_.stop && !slot_.abandoned) {
            if (!purged && !Live(state_, slot_, job)) {
                purged = true;
                CallTimer timer(state_, slot_);
                hr = voice_->Speak(nullptr, SpeakFlags | SPF_PURGEBEFORESPEAK, nullptr);
                if (FAILED(hr)) break;
            }
            const HANDLE events[] = {slot_.wake.get(), finished};
            DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0) continue;
            if (wait != WAIT_OBJECT_0 + 1) {
                hr = E_FAIL;
                break;
            }
            {
                CallTimer timer(state_, slot_);
                hr = voice_->WaitUntilDone(0);
                finishedSpeaking = hr == S_OK;
                if (hr == S_OK && !purged && Live(state_, slot_, job)) {
                    SPVOICESTATUS status{};
                    hr = voice_->GetStatus(&status, nullptr);
                    if (SUCCEEDED(hr) && FAILED(status.hrLastResult)) hr = status.hrLastResult;
                }
            }
            if (hr != S_FALSE) break;
        }
        {
            CallTimer timer(state_, slot_);
            // Shutdown or retirement can wake the wait without an ordinary
            // Cancel. Let the owning worker finish the purge before releasing
            // its voice. Shutdown on the caller never waits for this worker.
            if (!finishedSpeaking && !Live(state_, slot_, job)) {
                if (!purged)
                    hr = voice_->Speak(nullptr, SpeakFlags | SPF_PURGEBEFORESPEAK, nullptr);
                if (SUCCEEDED(hr)) voice_->WaitUntilDone(10000);
            }
            output->seal();
        }
        if (!Live(state_, slot_, job)) return S_FALSE;
        if (SUCCEEDED(hr) && (!capture->received() || (!job && !capture->audible()))) {
            // A warm-up of "Ready." must produce usable speech. A user's
            // punctuation-only request can legitimately contain only silence.
            return HRESULT_FROM_WIN32(ERROR_NO_DATA);
        }
        if (SUCCEEDED(hr)) complete = capture->finish();
        return hr;
    }

private:
    struct ReleaseOutput {
        void operator()(OutputStream* stream) const { stream->Release(); }
    };

    static std::unique_ptr<OutputStream, ReleaseOutput> makeOutput(std::shared_ptr<Capture> capture, Format format) {
        return std::unique_ptr<OutputStream, ReleaseOutput>(new OutputStream(std::move(capture), format.channels));
    }

    HRESULT attach(OutputStream& output, Format format) {
        HRESULT hr = output.marshal();
        ComPtr<ISpStream> stream;
        if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_INPROC_SERVER,
                                                IID_ISpStream, stream.voidOut());
        const auto wave = format.wave();
        if (SUCCEEDED(hr)) hr = stream->SetBaseStream(&output, WaveFormatId, &wave);
        if (SUCCEEDED(hr)) hr = voice_->SetOutput(stream.get(), FALSE);
        return hr;
    }

    HRESULT open(const Selection& selected) {
        HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_ISpVoice, voice_.voidOut());
        auto output = makeOutput({}, selected.choice.format);
        if (SUCCEEDED(hr)) hr = attach(*output, selected.choice.format);
        if (SUCCEEDED(hr)) hr = voice_->SetVoice(selected.token.get());
        return hr;
    }

    const std::shared_ptr<State>& stateOwner_;
    const std::shared_ptr<Slot>& slotOwner_;
    State& state_;
    Slot& slot_;
    Choice choice_;
    ComPtr<ISpVoice> voice_;
};

void Worker(const std::shared_ptr<State>& state, const std::shared_ptr<Slot>& slot) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ComApartment com(COINIT_MULTITHREADED);
    slot->callAt = NoDeadline;
    if (FAILED(com.result)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        slot->error = com.result;
        slot->failed = true;
        state->dispatch.signal();
        return;
    }
    Engine engine(state, slot);
    unsigned applied = 0;
    while (Live(*state, *slot, {})) {
        std::optional<Settings> settings;
        std::shared_ptr<Job> job;
        unsigned revision = 0;
        bool probe = false;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (slot->job && !slot->done) job = slot->job;
            else if (!slot->job && (slot->revision != applied || slot->probePreferred)) {
                settings = slot->settings;
                revision = slot->revision;
                probe = std::exchange(slot->probePreferred, false);
            }
        }
        if (settings) {
            const std::vector<std::wstring> none;
            const HRESULT hr = engine.configure(*settings, !probe && revision == slot->excludedRevision ? slot->excluded : none);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!Live(*state, *slot, {})) break;
                slot->progressDeadline = NoDeadline;
                if (hr == S_OK) {
                    slot->choice = engine.choice();
                    slot->readyRevision = applied = revision;
                } else {
                    slot->error = hr;
                    slot->failed = true;
                }
            }
            state->dispatch.signal();
            if (hr != S_OK) break;
        } else if (job) {
            std::shared_ptr<CachePcm> complete;
            const HRESULT hr = engine.speak(job, complete);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (!Live(*state, *slot, {})) break;
                slot->error = hr;
                if (FAILED(hr)) {
                    slot->failed = true;
                } else {
                    slot->done = true;
                    slot->complete = std::move(complete);
                }
            }
            state->dispatch.signal();
            if (FAILED(hr)) break;
            // The scheduler acknowledges completion before reusing the slot.
            WaitForSingleObject(slot->wake.get(), INFINITE);
        } else {
            WaitForSingleObject(slot->wake.get(), INFINITE);
        }
    }
}

class Scheduler {
    struct CacheCandidate {
        std::shared_ptr<Job> job;
        std::shared_ptr<CachePcm> pcm;
    };

public:
    explicit Scheduler(const std::shared_ptr<State>& state)
        : state_(state), cache_(std::make_shared<PhraseCache>(state->config.cacheMegabytes)) {}
    ~Scheduler() {
        std::list<Chunk> discardedAudio;
        std::list<std::shared_ptr<Job>> discardedJobs;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            discardedAudio.swap(state_->retiredAudio);
            discardedJobs.swap(state_->retiredJobs);
            for (auto& channel : slots_) {
                if (!channel.slot) continue;
                channel.slot->abandoned = true;
                channel.slot->wake.signal();
            }
            state_->space.notify_all();
        }
        candidates_.clear();
        cache_->clear();
    }

    void run() {
        NarratorWatch watch;
        bool settingsChanged = state_->config.followNarrator;
        bool watching = false;
        Time pollSettings = NoDeadline;
        while (!state_->stop) {
            const auto now = Clock::now();
            if (settingsChanged || now >= pollSettings) {
                settingsChanged = false;
                watching = watch.arm();
                pollSettings = watching ? NoDeadline : now + 900ms;
                Settings fresh = ReadSettings();
                if (!(fresh == settings_)) {
                    settings_ = std::move(fresh);
                    ++revision_;
                    choice_.reset();
                }
            }
            Time next = pollSettings;

            std::list<std::shared_ptr<Job>> retiredJobs;
            std::list<Chunk> retiredAudio;
            std::list<CacheCandidate> played;
            std::list<CacheCandidate> discarded;
            std::shared_ptr<Job> pending;
            std::shared_ptr<Job> dispatched;
            std::vector<HRESULT> errors;
            bool abandoned = false;
            std::optional<Choice> newChoice;
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (state_->stop) break;
                retiredJobs.swap(state_->retiredJobs);
                retiredAudio.swap(state_->retiredAudio);
                for (auto& channel : slots_) inspect(channel, now, next, errors, abandoned);
                const Choice* best = nullptr;
                for (const auto& channel : slots_) {
                    if (!channel.slot || channel.slot->readyRevision != revision_) continue;
                    const auto& candidate = channel.slot->choice;
                    if (!best || candidate.priority < best->priority) best = &candidate;
                }
                if (!best) {
                    choice_.reset();
                } else if (!choice_ || best->id != choice_->id || best->rate != choice_->rate ||
                           best->format != choice_->format) {
                    choice_ = *best;
                    state_->outputFormat = best->format;
                    state_->render.signal();
                    if (!lastLogged_ || best->id != lastLogged_->id || best->rate != lastLogged_->rate) {
                        lastLogged_ = newChoice = choice_;
                    }
                }
                checkPreferred(now, next);
                collectCache(now, next, played, discarded);
                if (choice_ && !state_->active && !state_->jobs.empty()) pending = state_->jobs.front();
            }
            retiredJobs.clear();
            retiredAudio.clear();
            discarded.clear();
            for (auto& candidate : played) cache_->insert(std::move(candidate.pcm));
            played.clear();
            if (pending) {
                const auto cached = cache_->find(*choice_, *pending);
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (!state_->stop && !state_->active && !state_->jobs.empty() && state_->jobs.front() == pending)
                    dispatched = dispatch(pending, cached);
            }
            retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                [](const auto& slot) { return slot->exited.load(); }), retired_.end());
            for (auto& channel : slots_) start(channel, now, next, errors);
            if (abandoned) state_->report(LogLevel::Error,
                L"A SAPI voice timed out and was abandoned. Recovery is pending.", L"voice-timeout");
            for (HRESULT hr : errors) {
                const auto key = L"sapi-" + ErrorCode(hr);
                state_->report(LogLevel::Error, L"SAPI failed (" + ErrorCode(hr) + L").", key.c_str());
            }
            if (newChoice) {
                state_->report(LogLevel::Info,
                    std::wstring(newChoice->fallback ? L"Fallback voice: " : L"Voice: ") + newChoice->name +
                    L". SAPI rate " + std::to_wstring(newChoice->rate) + L". PCM " +
                    std::to_wstring(newChoice->format.rate) + L" Hz, " +
                    std::to_wstring(newChoice->format.channels) + L" channel(s).");
            }
            if (dispatched) state_->report(LogLevel::Debug, L"Speak: " + dispatched->text);
            pending.reset();
            dispatched.reset();
            HANDLE events[] = {watch.event(), state_->dispatch.get()};
            if (watching) {
                settingsChanged = WaitForMultipleObjects(2, events, FALSE, WaitTime(next)) == WAIT_OBJECT_0;
            } else {
                WaitForSingleObject(state_->dispatch.get(), WaitTime(next));
            }
        }
    }

private:
    struct Channel {
        std::shared_ptr<Slot> slot;
        Time restart{};
        std::vector<std::wstring> excluded;
        unsigned excludedRevision = 0;
    };

    // Recheck a missing voice on one idle worker. Its partner remains
    // available for a new request throughout enumeration and warming.
    void checkPreferred(Time now, Time& next) {
        auto* first = slots_[0].slot.get();
        auto* second = slots_[1].slot.get();
        if (!first || !second || first->readyRevision != revision_ || second->readyRevision != revision_) return;
        auto* lower = first->choice.priority > second->choice.priority ? first : second;
        const bool differentRank = first->choice.priority != second->choice.priority;
        if (!differentRank && !lower->choice.fallback &&
            (settings_.narratorVoice.empty() || lower->choice.priority == 0)) {
            probeAfter_ = NoDeadline;
            return;
        }
        if (probeAfter_ == NoDeadline) probeAfter_ = now + probeInterval_;
        if (now < probeAfter_) {
            next = std::min(next, probeAfter_);
        } else if (!lower->job && !state_->active && state_->jobs.empty()) {
            lower->readyRevision = 0;
            lower->probePreferred = true;
            lower->wake.signal();
            probeAfter_ = now + probeInterval_;
        }
    }

    void inspect(Channel& channel, Time now, Time& next,
                 std::vector<HRESULT>& errors, bool& abandoned) {
        if (channel.slot) {
            auto& slot = *channel.slot;
            const Time first = slot.firstDeadline;
            const Time call = slot.callAt;
            const Time deadline = std::min({first, slot.progressDeadline.load(),
                                            call == NoDeadline ? NoDeadline : call + 10s});

            if (slot.done) {
                auto job = std::move(slot.job);
                if (slot.complete) candidates_.push_back({job, std::move(slot.complete)});
                if (state_->active == job) {
                    if (slot.error == S_OK) {
#ifdef VB_VOICE_TEST
                        test::Observe({test::Kind::Complete, job->id, job->generation});
#endif
                    } else if (FAILED(slot.error)) {
                        errors.push_back(slot.error);
                    }
                    state_->active.reset();
                }
                slot.error = S_OK;
                slot.done = false;
                slot.purgeRequested = false;
                slot.firstDeadline = NoDeadline;
                slot.progressDeadline = NoDeadline;
                slot.wake.signal();
            } else if (slot.failed || now >= deadline) {
                if (slot.failed) errors.push_back(slot.error);
                else {
                    abandoned = true;
#ifdef VB_VOICE_TEST
                    test::Observe({test::Kind::Abandoned, slot.job ? slot.job->id : 0});
#endif
                }
                if (slot.job && state_->active == slot.job) {
                    // A retry is safe only before anything has reached the output queues.
                    if (!slot.job->published &&
                        (!slot.failed || ++slot.job->failedTokens[slot.attemptedId] <= 2))
                        state_->jobs.push_front(slot.job);
                    state_->active.reset();
                }
                slot.abandoned = true;
                slot.wake.signal();
                state_->space.notify_all();
                channel.restart = slot.failed ? now + 1s : now;
                if (channel.excludedRevision != slot.revision) channel.excluded.clear();
                if (!slot.attemptedId.empty() &&
                    std::find(channel.excluded.begin(), channel.excluded.end(), slot.attemptedId) == channel.excluded.end())
                    channel.excluded.push_back(slot.attemptedId);
                channel.excludedRevision = slot.revision;
                slot.job.reset();
                retired_.push_back(std::move(channel.slot));
            } else {
                next = std::min(next, deadline);
                if (slot.job && slot.job->generation != state_->generation && !slot.purgeRequested) {
                    slot.purgeRequested = true;
                    slot.firstDeadline = NoDeadline;
                    slot.progressDeadline = now + 10s;
                    next = std::min(next, slot.progressDeadline.load());
                    slot.wake.signal();
                }
            }
        }
        if (channel.slot && channel.slot->revision != revision_) {
            channel.slot->settings = settings_;
            channel.slot->revision = revision_;
            channel.slot->wake.signal();
        }
    }

    void start(Channel& channel, Time now, Time& next, std::vector<HRESULT>& errors) {
        if (channel.slot || state_->stop) return;
        // An in-process COM call cannot safely be killed. Keep at most four
        // workers, including any still inside Release or CoUninitialize.
        const size_t active = (slots_[0].slot ? 1U : 0U) + (slots_[1].slot ? 1U : 0U);
        if (retired_.size() + active >= 4) return;
        if (now < channel.restart) {
            next = std::min(next, channel.restart);
            return;
        }
        auto slot = std::make_shared<Slot>(cache_);
        slot->settings = settings_;
        slot->revision = revision_;
        slot->excluded = channel.excluded;
        slot->excludedRevision = channel.excludedRevision;
        slot->callAt = now;  // Includes thread startup and CoInitializeEx.
        next = std::min(next, now + 10s);
        try {
            auto state = state_;
            StartVoiceThread(L"VoiceBridge SAPI", [state, slot] {
#ifdef VB_VOICE_TEST
                test::Observe({test::Kind::WorkerStarted});
#endif
                try {
                    Worker(state, slot);
                } catch (...) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    slot->error = E_FAIL;
                    slot->failed = true;
                    state->dispatch.signal();
                }
                slot->exited = true;  // All worker-owned COM objects and the apartment are gone.
                state->dispatch.signal();
            });
            channel.slot = std::move(slot);
        } catch (...) {
            errors.push_back(E_OUTOFMEMORY);
            channel.restart = now + 1s;
            next = std::min(next, channel.restart);
        }
    }

    // Inspect playback state under its lock, then release or index the PCM
    // outside it so allocation and eviction never delay RPC callers.
    void collectCache(Time now, Time& next, std::list<CacheCandidate>& played,
                      std::list<CacheCandidate>& discarded) {
        for (auto it = candidates_.begin(); it != candidates_.end();) {
            const auto current = it++;
            const auto& job = *current->job;
            if (job.generation != state_->generation) {
                discarded.splice(discarded.end(), candidates_, current);
            } else if (!job.bufferedFrames && job.submittedUntil != Time{}) {
                if (now >= job.submittedUntil) {
                    played.splice(played.end(), candidates_, current);
                } else {
                    next = std::min(next, job.submittedUntil);
                }
            }
        }
    }

    std::shared_ptr<Job> dispatch(const std::shared_ptr<Job>& job, const std::shared_ptr<const Samples>& cached) {
        if (cached) {
            const auto frames = cached->size() / choice_->format.channels;
            if (state_->waiting + choice_->format.seconds(frames) > BufferSeconds) return {};
            state_->jobs.pop_front();
#ifdef VB_VOICE_TEST
            test::Observe({test::Kind::CacheHit, job->id, job->generation});
            test::Observe({test::Kind::Complete, job->id, job->generation});
#endif
            state_->queueChunk({cached, 0, frames, choice_->format, job->id, job->generation, job});
            if (!state_->jobs.empty()) state_->dispatch.signal();
            return job;
        }

        const auto preferred = lastSlot_ >= 0 && lastGeneration_ == job->generation ? lastSlot_ :
                               lastSlot_ == 0 ? 1 : 0;
        for (int index : {preferred, 1 - preferred}) {
            auto* slot = slots_[index].slot.get();
            if (!slot || slot->job || slot->readyRevision != revision_ || slot->choice.id != choice_->id) continue;
            state_->jobs.pop_front();
            state_->active = slot->job = job;
            slot->firstDeadline = Clock::now() + 2s;
            lastSlot_ = index;
            lastGeneration_ = job->generation;
            slot->wake.signal();
            state_->dispatch.signal();
            return job;
        }
        return {};
    }

    std::shared_ptr<State> state_;
    std::shared_ptr<PhraseCache> cache_;
    std::list<CacheCandidate> candidates_;
    Channel slots_[2];
    std::vector<std::shared_ptr<Slot>> retired_;
    Settings settings_;
    unsigned revision_ = 1;
    Time probeAfter_ = NoDeadline;
#ifdef VB_VOICE_TEST
    const std::chrono::seconds probeInterval_{test::Flag("VOICEBRIDGE_TEST_REPROBE") ? 3 : 30};
#else
    static constexpr auto probeInterval_ = 30s;
#endif
    std::optional<Choice> choice_;
    std::optional<Choice> lastLogged_;
    int lastSlot_ = -1;
    std::uint64_t lastGeneration_ = 0;
};

}  // namespace

#ifdef VB_VOICE_TEST
void test::SetTestStalls(unsigned count) {
    std::lock_guard<std::mutex> lock(stallMutex);
    stallCount = count;
}

void test::ReleaseTestStalls() {
    std::lock_guard<std::mutex> lock(stallMutex);
    stallCount = 0;
    ++stallEpoch;
    stallChanged.notify_all();
}
#endif

void RunSynthesis(const std::shared_ptr<State>& state) {
    Scheduler(state).run();
}

}  // namespace vb::voice
