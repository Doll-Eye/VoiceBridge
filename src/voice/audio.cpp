#include "voice_internal.h"

#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace vb::voice {
namespace {

constexpr auto Forever = Time::max();
// Cancel may add a 5 ms fade before clamping this deadline to its own limit.
constexpr auto PrimedDeadline = Forever - 5ms;

Time Advance(Time time, double seconds) {
    return time + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

class EndpointChanges final : public IMMNotificationClient {
public:
    explicit EndpointChanges(std::weak_ptr<State> state) : state_(std::move(state)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IMMNotificationClient)) return E_NOINTERFACE;
        *result = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        auto count = --references_;
        if (!count) delete this;
        return count;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole && enabled) {
            changed = true;
            if (auto state = state_.lock()) state->render.signal();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    std::atomic<bool> changed{false};
    std::atomic<bool> enabled{true};
private:
    std::weak_ptr<State> state_;
    std::atomic<ULONG> references_{1};
};

class AudioDevice {
public:
    explicit AudioDevice(const std::shared_ptr<State>& state) : state_(state) {}
    ~AudioDevice() {
        close();
        if (notifications_) {
            notifications_->enabled = false;
            const HRESULT hr = registered_
                ? enumerator_->UnregisterEndpointNotificationCallback(notifications_) : S_OK;
            enumerator_.reset();
            // MMDevice does not retain a reference. Keep a disabled callback
            // alive if an unexpected failure leaves its registration uncertain.
            if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) notifications_->Release();
        }
    }

    bool changed() { return notifications_ && notifications_->changed.exchange(false); }

    HRESULT open(Format format) {
        close();
        if (!event_.get()) return HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE);
        HRESULT hr = S_OK;
        if (!enumerator_) {
            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IMMDeviceEnumerator), enumerator_.voidOut());
            if (FAILED(hr)) return hr;
        }
        if (!registered_) {
            if (!notifications_) notifications_ = new EndpointChanges(state_);
            hr = enumerator_->RegisterEndpointNotificationCallback(notifications_);
            registered_ = SUCCEEDED(hr);
            if (!registered_) state_->report(LogLevel::Error,
                L"The audio device change listener could not start (" + ErrorCode(hr) + L").", L"device-listener");
        }

        ComPtr<IMMDevice> endpoint;
        hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, endpoint.out());
        if (FAILED(hr)) return hr;
        hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, client_.voidOut());
        if (FAILED(hr)) return hr;
        const auto wave = format.wave();
        constexpr DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &wave, nullptr);
        if (SUCCEEDED(hr)) hr = client_->SetEventHandle(event_.get());
        if (SUCCEEDED(hr)) hr = client_->GetBufferSize(&capacity_);
        if (SUCCEEDED(hr)) hr = client_->GetService(__uuidof(IAudioRenderClient), render_.voidOut());
        if (FAILED(hr)) {
            close();
            return hr;
        }

#ifndef VB_VOICE_TEST
        if (!state_->config.silentOutput && state_->config.volume > 0) {
            ComPtr<ISimpleAudioVolume> volume;
            BOOL muted = FALSE;
            if (SUCCEEDED(client_->GetService(__uuidof(ISimpleAudioVolume), volume.voidOut())) &&
                SUCCEEDED(volume->GetMute(&muted)) && muted && SUCCEEDED(volume->SetMute(FALSE, nullptr))) {
                state_->report(LogLevel::Info,
                    L"The VoiceBridge audio session was muted and has been unmuted.", L"muted");
            }
        }
#endif

        auto id = deviceId(*endpoint.get());
        if (!readyBefore_ || id != lastDevice_) {
            state_->report(LogLevel::Info, L"Audio device: " + deviceName(*endpoint.get(), id) + L".");
            lastDevice_ = std::move(id);
            readyBefore_ = true;
        }
        format_ = format;
#ifdef VB_VOICE_TEST
        test::Observe({test::Kind::DeviceOpened, 0, 0, static_cast<double>(capacity_), nullptr, 0,
                       format.rate, format.channels});
#endif
        return S_OK;
    }

    void close() {
        stop();
        render_.reset();
        client_.reset();
    }
    HRESULT stop() {
        if (!started_) return S_OK;
        started_ = false;
        return client_->Stop();
    }
    HRESULT start() {
        if (started_) return S_OK;
        ResetEvent(event_.get());
        HRESULT hr = client_->Start();
        started_ = SUCCEEDED(hr);
        return hr;
    }

    bool available() const { return static_cast<bool>(render_); }
    bool started() const { return started_; }
    bool watchesChanges() const { return registered_; }
    Format format() const { return format_; }
    HANDLE event() const { return event_.get(); }
    size_t target() const { return std::min<size_t>(capacity_, format_.frames(20)); }
    HRESULT padding(UINT32& count) const { return client_->GetCurrentPadding(&count); }
    HRESULT get(size_t frames, BYTE** bytes) const { return render_->GetBuffer(static_cast<UINT32>(frames), bytes); }
    HRESULT submit(size_t frames) const { return render_->ReleaseBuffer(static_cast<UINT32>(frames), 0); }
    bool stillDefault() const {
        ComPtr<IMMDevice> endpoint;
        if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, endpoint.out()))) return false;
        return deviceId(*endpoint.get()) == lastDevice_;
    }

private:
    static std::wstring deviceId(IMMDevice& endpoint) {
        std::wstring id;
        LPWSTR value = nullptr;
        if (SUCCEEDED(endpoint.GetId(&value)) && value) id = value;
        CoTaskMemFree(value);
        return id;
    }

    static std::wstring deviceName(IMMDevice& endpoint, std::wstring name) {
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(endpoint.OpenPropertyStore(STGM_READ, properties.out()))) {
            PROPVARIANT friendly{};
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &friendly)) &&
                friendly.vt == VT_LPWSTR && friendly.pwszVal) name = friendly.pwszVal;
            PropVariantClear(&friendly);
        }
        if (name.empty()) name = L"Default console render device";
        return name;
    }

    std::shared_ptr<State> state_;
    Event event_;
    ComPtr<IMMDeviceEnumerator> enumerator_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    EndpointChanges* notifications_ = nullptr;
    bool registered_ = false;
    bool started_ = false;
    bool readyBefore_ = false;
    Format format_;
    UINT32 capacity_ = 0;
    std::wstring lastDevice_;
};

class AudioThreadPriority {
public:
    AudioThreadPriority() {
        DWORD task = 0;
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    }
    ~AudioThreadPriority() { if (handle_) AvRevertMmThreadCharacteristics(handle_); }
private:
    HANDLE handle_ = nullptr;
};

class Renderer {
public:
    Renderer(const std::shared_ptr<State>& state, bool useDevice)
        : state_(state), device_(state), useDevice_(useDevice),
          gain_(std::clamp(state->config.volume, 0, 100)),
          keepAwake_(std::chrono::seconds(std::max(0, state->config.keepAwakeSeconds))) {}

    void run() {
        struct Timer {
            HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                   TIMER_MODIFY_STATE | SYNCHRONIZE);
            ~Timer() { if (handle) CloseHandle(handle); }
        } timer;
        while (!state_->stop) {
            Time deadline = step();
            // While the stream is kept warm with no data, its periodic event
            // has no work to do. Speech and endpoint changes signal render.
            const bool rendering = device_.started() && deviceFrames_;
            // A disconnected stream may stop signaling its event entirely.
            const Time watchdog = rendering ? Clock::now() + 100ms : Forever;
            deadline = std::min(deadline, watchdog);
            HANDLE events[] = {state_->render.get(), device_.event(), nullptr};
#ifdef VB_VOICE_TEST
            DWORD count = rendering && !testMissingRenderEvent_ ? 2 : 1;
#else
            DWORD count = rendering ? 2 : 1;
#endif
            DWORD timeout = WaitTime(deadline);
            if (timer.handle && !device_.available() && deadline <= until_ && timeout) {
                using Ticks = std::chrono::duration<LONGLONG, std::ratio<1, 10000000>>;
                LARGE_INTEGER due;
                due.QuadPart = -std::max<LONGLONG>(1, std::chrono::ceil<Ticks>(deadline - Clock::now()).count());
                if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
                    events[count++] = timer.handle;
                    timeout = INFINITE;
                }
            }
            const auto wait = WaitForMultipleObjects(count, events, FALSE, timeout);
#ifdef VB_VOICE_TEST
            if (wait == WAIT_TIMEOUT && rendering && deadline == watchdog) {
                test::Observe({test::Kind::DeviceWatchdog});
            }
#else
            (void)wait;
#endif
        }
    }

private:
    Time step() {
        auto now = Clock::now();
        bool changed = device_.changed();
        if (useDevice_ && device_.available() && !device_.watchesChanges() && now >= pollDevice_) {
            changed = !device_.stillDefault();
            pollDevice_ = now + 400ms;
        }
        if (changed) {
            closeDevice(true);
            retryDevice_ = now;
        }

        Chunk next = front();
        std::optional<Format> wanted;
        if (useDevice_ && !device_.available()) {
            std::lock_guard<std::mutex> lock(state_->mutex);
            wanted = next.pcm ? std::optional<Format>(next.format) : state_->outputFormat;
        }
#ifdef VB_VOICE_TEST
        if (injectAt_ != Forever && now >= injectAt_) {
            closeDevice(true);
            injectAt_ = Forever;
            recoverAt_ = now + 100ms;
            test::Observe({test::Kind::DeviceLost, next.id, state_->generation.load()});
        }
        if (recoverAt_ != Forever && now >= recoverAt_) {
            retryDevice_ = now;
            test::Observe({test::Kind::DeviceRestored, next.id, state_->generation.load()});
            recoverAt_ = Forever;
        }
        const bool simulateLoss = recoverAt_ != Forever;
#else
        constexpr bool simulateLoss = false;
#endif
        if (wanted && !simulateLoss && now >= retryDevice_) {
            format_ = *wanted;
            openDevice();
        }

        now = Clock::now();
        UINT32 padding = 0;
        const bool hardware = device_.available();
        if (hardware) {
            if (device_.started()) {
                const HRESULT hr = device_.padding(padding);
                if (FAILED(hr)) return failedDevice(hr);
#ifdef VB_VOICE_TEST
                if (testTraceDevice_ && !padding && deviceFrames_ && next.pcm && next.format == format_ &&
                    next.generation == generation_ && state_->generation == generation_) {
                    test::Observe({test::Kind::RenderStarved, next.id, generation_});
                }
#endif
            } else {
                padding = static_cast<UINT32>(deviceFrames_);
            }
            retainPadding(padding);
            until_ = padding ? Advance(now, format_.seconds(padding)) : Time{};
        } else if (now < until_) {
            padding = static_cast<UINT32>(std::ceil(std::chrono::duration<double>(until_ - now).count() * format_.rate));
        }

        const bool primed = hardware && !device_.started() && deviceFrames_;
        if (observeCancellation(now, primed ? 0 : padding) && primed) {
            closeDevice();
            return Clock::now();
        }
        updateDeadline();

        if (next.pcm && next.format != format_ && !padding && !fadeFrames_) {
            format_ = next.format;
            lastFrame_.clear();
            if (hardware) {
                openDevice();
                return now;
            }
        }

        const auto target = hardware ? device_.target() : format_.frames(20);
        if (hardware || padding <= format_.frames(9)) {
            for (size_t available = target > padding ? target - padding : 0; available && !state_->stop;) {
                if (fadeFrames_) {
                    size_t count = std::min(available, fadeFrames_);
                    const auto faded = fade(count);
                    const HRESULT hr = submit(faded, count, nullptr);
                    if (FAILED(hr)) return failedDevice(hr);
                    if (hr == S_FALSE) return Clock::now();
                    fadeFrames_ -= count;
                    available -= count;
                    continue;
                }
                next = front();
                if (!next.pcm || next.format != format_) break;
                size_t count = std::min(available, next.count);
                const auto pcm = scaled(next, count);
                const HRESULT hr = submit(pcm, count, &next);
                if (FAILED(hr)) return failedDevice(hr);
                if (hr == S_FALSE) return Clock::now();
                available -= count;
            }
        }

        now = Clock::now();
        if (hardware && !device_.started() && deviceFrames_) {
#ifdef VB_VOICE_TEST
            if (testDelayDeviceStart_) Sleep(30);
#endif
            if (state_->stop || generation_ != state_->generation) {
                closeDevice();
                return Clock::now();
            }
            const HRESULT hr = device_.start();
            if (FAILED(hr)) return failedDevice(hr);
            now = Clock::now();
            until_ = Advance(now, format_.seconds(deviceFrames_));
            publishStart(now);
#ifdef VB_VOICE_TEST
            test::Observe({test::Kind::DeviceStarted, 0, generation_, static_cast<double>(deviceFrames_),
                           nullptr, 0, format_.rate, format_.channels});
#endif
        }

        Time deadline = Forever;
        if (!hardware && wanted && !simulateLoss) deadline = retryDevice_;
        if (hardware && !device_.watchesChanges()) deadline = pollDevice_;
#ifdef VB_VOICE_TEST
        deadline = std::min({deadline, injectAt_, recoverAt_});
#endif
        next = front();
        if (hardware && device_.started() && !next.pcm && !fadeFrames_) {
            const auto idleDeadline = lastWrite_ + keepAwake_;
            if (now >= idleDeadline && !deviceFrames_) {
                const HRESULT hr = device_.stop();
                if (FAILED(hr)) return failedDevice(hr);
#ifdef VB_VOICE_TEST
                test::Observe({test::Kind::IdleStopped});
#endif
            } else if (now < idleDeadline) {
                deadline = std::min(deadline, idleDeadline);
            }
        }
        if (!hardware && (next.pcm || fadeFrames_) && now < until_) {
            const Time nextWrite = next.pcm && next.format != format_ ? until_ : Advance(until_, -format_.seconds(format_.frames(9)));
            deadline = std::min(deadline, std::max(now, nextWrite));
        }
        return deadline;
    }

    Chunk front() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->audio.empty() ? Chunk{} : state_->audio.front();
    }

    bool observeCancellation(Time now, size_t padding) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto current = state_->generation.load();
        if (current == generation_) return false;
        generation_ = current;
        if (!padding || lastFrame_.empty()) {
            fadeFrames_ = 0;
        } else if (hasSpeech_ && !fadeFrames_) {
            fadeFrames_ = format_.frames(5);
        }
        hasSpeech_ = false;
        state_->audibleUntil = fadeFrames_ ? Advance(std::max(now, until_), format_.seconds(fadeFrames_)) :
            padding ? until_ : Time{};
        return true;
    }

    void updateDeadline() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (generation_ == state_->generation && !state_->stop) {
            if (deviceFrames_ && !device_.started()) {
                state_->audibleUntil = PrimedDeadline;
            } else {
                state_->audibleUntil = fadeFrames_
                    ? Advance(std::max(Clock::now(), until_), format_.seconds(fadeFrames_))
                    : until_;
            }
        }
    }

    void publishStart(Time start) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (generation_ != state_->generation || state_->stop) return;
        size_t frames = 0;
        for (auto& packet : devicePackets_) {
            frames += packet.count;
            if (packet.job) packet.job->submittedUntil = Advance(start, format_.seconds(frames));
        }
        state_->audibleUntil = until_;
        state_->dispatch.signal();
    }

    Samples fade(size_t frames) const {
        Samples pcm(frames * format_.channels);
        const size_t total = format_.frames(5);
        for (size_t frame = 0; frame < frames; ++frame) {
            const auto remaining = static_cast<int>(fadeFrames_ - frame - 1);
            for (size_t ch = 0; ch < format_.channels; ++ch) {
                pcm[frame * format_.channels + ch] =
                    static_cast<Sample>(lastFrame_[ch] * remaining / static_cast<int>(total));
            }
        }
        return pcm;
    }

    Samples scaled(const Chunk& chunk, size_t frames) const {
        const auto begin = chunk.pcm->begin() + chunk.offset * format_.channels;
        Samples pcm(begin, begin + frames * format_.channels);
        if (gain_ != 100) {
            for (auto& sample : pcm) sample = static_cast<Sample>(sample * gain_ / 100);
        }
        return pcm;
    }

    HRESULT submit(const Samples& pcm, size_t frames, const Chunk* chunk) {
        const bool hardware = device_.available();
        if (hardware) {
            BYTE* bytes = nullptr;
            const HRESULT hr = device_.get(frames, &bytes);
            if (FAILED(hr)) return hr;
            const auto size = frames * format_.channels * sizeof(Sample);
            if (state_->config.silentOutput) std::memset(bytes, 0, size);
            else std::memcpy(bytes, pcm.data(), size);
        }

        // The generation check and publication are one operation with respect to
        // Cancel. Acquiring and filling the WASAPI buffer happens outside it.
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto* head = state_->audio.empty() ? nullptr : &state_->audio.front();
        if (state_->stop || generation_ != state_->generation ||
            (chunk && (!head || head->id != chunk->id || head->pcm != chunk->pcm ||
                       head->offset != chunk->offset || head->count < frames))) {
            // A concurrent tail trim can shorten even the head chunk. WASAPI
            // allows discarding this packet, but not a partial release.
            if (hardware) {
                const auto hr = device_.submit(0);
                if (FAILED(hr)) return hr;
            }
            return S_FALSE;
        }
        // Commit before the first handoff. A crash inside ReleaseBuffer may cut
        // this phrase short, but a replacement must never repeat it.
        if (chunk) state_->retire(*chunk->job);
        if (hardware) {
            const auto hr = device_.submit(frames);
            if (FAILED(hr)) return hr;
        }
        if (hardware) {
            auto packet = chunk ? *chunk : Chunk{};
            packet.count = frames;
            devicePackets_.push_back(std::move(packet));
            deviceFrames_ += frames;
        }
        lastWrite_ = Clock::now();
        until_ = Advance(std::max(lastWrite_, until_), format_.seconds(frames));
        const bool priming = hardware && !device_.started();
        state_->audibleUntil = priming ? PrimedDeadline : until_;
        if (chunk) {
            hasSpeech_ = true;
            lastFrame_.assign(pcm.begin() + (frames - 1) * format_.channels, pcm.begin() + frames * format_.channels);
            chunk->job->submittedUntil = priming ? Time{} : until_;
            state_->consume(frames);
        }
#ifdef VB_VOICE_TEST
        if (hardware && testTraceDevice_) {
            test::Observe({test::Kind::HardwareQueued, chunk ? chunk->id : 0, generation_,
                           format_.seconds(deviceFrames_) * 1000});
        }
        test::Observe({chunk ? test::Kind::Packet : test::Kind::Fade, chunk ? chunk->id : 0, generation_,
            std::chrono::duration<double, std::milli>(until_.time_since_epoch()).count(),
            pcm.data(), frames * format_.channels, format_.rate, format_.channels,
            chunk ? chunk->job->text.c_str() : nullptr});
        if (chunk && injectAt_ == Forever && recoverAt_ == Forever) {
            const bool repeat = testRepeatedDeviceLoss_ && hardware && lastLossJob_ != chunk->id;
            if (repeat || (!injected_ && test::Flag("VOICEBRIDGE_TEST_DEVICE_LOSS"))) {
                injected_ = true;
                lastLossJob_ = chunk->id;
                injectAt_ = lastWrite_ + 100ms;
            }
        }
#endif
        return S_OK;
    }

    void retainPadding(size_t padding) {
        size_t played = deviceFrames_ > padding ? deviceFrames_ - padding : 0;
        while (played && !devicePackets_.empty()) {
            auto& packet = devicePackets_.front();
            const auto count = std::min(played, packet.count);
            played -= count;
            deviceFrames_ -= count;
            packet.offset += count;
            packet.count -= count;
            if (!packet.count) devicePackets_.pop_front();
        }
    }

    void closeDevice(bool recoverPending = false) {
        if (!device_.available()) return;
        if (recoverPending) {
            UINT32 padding = static_cast<UINT32>(deviceFrames_);
            // Freeze the old stream before sampling its unread frames so it
            // cannot play those same frames while they are being requeued.
            if (device_.started() && (FAILED(device_.stop()) || FAILED(device_.padding(padding)))) {
                const double secondsLeft = std::chrono::duration<double>(until_ - Clock::now()).count();
                padding = static_cast<UINT32>(std::ceil(std::max(0.0, secondsLeft) * format_.rate));
            }
            retainPadding(padding);
            std::lock_guard<std::mutex> lock(state_->mutex);
            // Hand any unplayed frames to the new device or real-time sink in
            // their original order. A dead endpoint can only supply an estimate.
            for (auto it = devicePackets_.rbegin(); it != devicePackets_.rend(); ++it) {
                if (!it->job || state_->stop || it->generation != state_->generation) continue;
                state_->waiting += it->format.seconds(it->count);
                it->job->bufferedFrames += it->count;
                state_->audio.push_front(*it);
            }
        }
        devicePackets_.clear();
        deviceFrames_ = 0;
        device_.close();
        until_ = {};
        fadeFrames_ = 0;
        lastFrame_.clear();
        hasSpeech_ = false;
    }

    void openDevice() {
        closeDevice();
        const HRESULT hr = device_.open(format_);
        if (FAILED(hr)) {
            retryDevice_ = Clock::now() + 2s;
            deviceError(hr);
        }
    }

    Time failedDevice(HRESULT hr) {
        closeDevice(true);
        const auto now = Clock::now();
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED && now >= immediateRetryAfter_) {
            retryDevice_ = now;
            immediateRetryAfter_ = now + 2s;
        } else {
            retryDevice_ = now + 2s;
        }
        deviceError(hr);
        return now;
    }

    void deviceError(HRESULT hr) {
        const auto key = L"device-" + ErrorCode(hr);
        state_->report(LogLevel::Error, L"Audio device unavailable (" + ErrorCode(hr) +
                       L"). Speech will advance in real time while the device is retried.", key.c_str());
    }

    std::shared_ptr<State> state_;
    AudioDevice device_;
    const bool useDevice_;
    const int gain_;
    const std::chrono::seconds keepAwake_;
    Format format_;
    Time retryDevice_{};
    Time immediateRetryAfter_{};
    Time pollDevice_{};
    Time until_{};
    Time lastWrite_{};
    std::uint64_t generation_ = 1;
    size_t fadeFrames_ = 0;
    bool hasSpeech_ = false;
    Samples lastFrame_;
    std::deque<Chunk> devicePackets_;
    size_t deviceFrames_ = 0;
#ifdef VB_VOICE_TEST
    const bool testDelayDeviceStart_ = test::Flag("VOICEBRIDGE_TEST_DELAY_DEVICE_START");
    const bool testMissingRenderEvent_ = test::Flag("VOICEBRIDGE_TEST_MISSING_RENDER_EVENT");
    const bool testTraceDevice_ = test::Flag("VOICEBRIDGE_TEST_TRACE_DEVICE");
    const bool testRepeatedDeviceLoss_ = test::Flag("VOICEBRIDGE_TEST_REPEATED_DEVICE_LOSS");
    bool injected_ = false;
    std::uint64_t lastLossJob_ = 0;
    Time injectAt_ = Forever;
    Time recoverAt_ = Forever;
#endif
};

}  // namespace

void RunAudio(const std::shared_ptr<State>& state) {
    ComApartment com(COINIT_MULTITHREADED);
    AudioThreadPriority priority;
    if (!state->config.nullOutput && FAILED(com.result)) {
        state->report(LogLevel::Error, L"Audio COM initialization failed (" + ErrorCode(com.result) + L").",
                      L"audio-com");
    }
    Renderer(state, !state->config.nullOutput && SUCCEEDED(com.result)).run();
}

}  // namespace vb::voice
