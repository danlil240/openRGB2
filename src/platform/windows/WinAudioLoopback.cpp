/*---------------------------------------------------------*\
||| WinAudioLoopback.cpp                                      ||
|||                                                           ||
|||   Qt-free port of the preserved plugin's               ||
|||   inputs/AudioLoopback.cpp (Windows WASAPI section)    ||
|||   onto the engine's IReactiveSource seam (Task 4.6).   ||
|||   Shared-mode loopback on the default render endpoint; ||
|||   ~5 ms energy chunks feed the OnsetDetect -> InputBus ||
|||   level + onset events. Raw audio never leaves the     ||
|||   engine process — only the smoothed level and onset   ||
|||   strengths cross into scene evaluation.               ||
|||                                                           ||
|||   capture_paused: the loop keeps draining packets so   ||
|||   no stale backlog bursts through on resume, but feeds ||
|||   nothing into the bus while paused. Disabled = thread ||
|||   stopped entirely (no hidden device cost).            ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "WinReactiveInputs.h"

#include "inputs/OnsetDetect.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace openrgb2
{
namespace platform
{

namespace
{

/*---------------------------------------------------------*\
||| Minimal IMMNotificationClient — only the default       ||
||| render-endpoint change matters, so the pump reopens.   ||
||| Stack-allocated: AddRef/Release never delete.          ||
\*---------------------------------------------------------*/
class DefaultDeviceNotifier : public IMMNotificationClient
{
public:
    explicit DefaultDeviceNotifier(std::atomic<bool>* flag)
        : reinit(flag) {}

    ULONG   AddRef()  override { return 1; }
    ULONG   Release() override { return 1; }
    HRESULT QueryInterface(REFIID riid, void** ppv) override
    {
        if(riid == __uuidof(IUnknown) ||
           riid == __uuidof(IMMNotificationClient))
        {
            *ppv = this;
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                   LPCWSTR) override
    {
        if(flow == eRender && role == eConsole)
            reinit->store(true);
        return S_OK;
    }
    HRESULT OnDeviceAdded(LPCWSTR) override              { return S_OK; }
    HRESULT OnDeviceRemoved(LPCWSTR) override            { return S_OK; }
    HRESULT OnDeviceStateChanged(LPCWSTR, DWORD) override{ return S_OK; }
    HRESULT OnPropertyValueChanged(LPCWSTR,
                                   const PROPERTYKEY) override
    {
        return S_OK;
    }

private:
    std::atomic<bool>* reinit;
};

/* Chunk energy: mean square over all channels. float32 and
   pcm16 interleaved covered; anything else -> false. */
bool ChunkEnergy(const BYTE* data, UINT32 frames,
                 const WAVEFORMATEX* wfx, DWORD flags,
                 float& energy)
{
    if((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 ||
       data == nullptr || frames == 0)
    {
        energy = 0.0f;
        return true;
    }
    const int ch = wfx->nChannels;
    double acc = 0.0;
    if(wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
       (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx)
            ->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
    {
        const float* s = reinterpret_cast<const float*>(data);
        for(UINT32 i = 0; i < frames * (UINT32)ch; i++)
            acc += (double)s[i] * s[i];
    }
    else if(wfx->wFormatTag == WAVE_FORMAT_PCM &&
            wfx->wBitsPerSample == 16)
    {
        const short* s = reinterpret_cast<const short*>(data);
        for(UINT32 i = 0; i < frames * (UINT32)ch; i++)
        {
            const double v = s[i] / 32768.0;
            acc += v * v;
        }
    }
    else
    {
        return false;
    }
    energy = (float)(acc / ((double)frames * ch));
    return true;
}

class WinAudioLoopback final : public IReactiveSource
{
public:
    ~WinAudioLoopback() override { Stop(); }

    bool Start(studio::InputBus* b) override
    {
        /* lifecycle_mu_ closes the check-then-spawn race:
           two concurrent Starts can never both pass the
           running check, and a Stop can never slip between
           the flag set and the thread_ assignment. */
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        if(running_.load())
            return false;
        /* Synchronous pre-flight: prove a loopback session
           CAN be opened right now so a genuinely unavailable
           source (no default render endpoint, init denied)
           refuses Start honestly — the reply contract
           promises Unavailable + stays-disabled. Failures
           that only appear mid-capture (device removed)
           still surface asynchronously via Status(). */
        std::string err;
        if(!Probe(err))
        {
            SetStatus(err);
            return false;
        }
        bus_       = b;
        running_   = true;
        reinit_    = false;
        onset_.Reset();
        thread_ = std::thread(&WinAudioLoopback::ThreadMain, this);
        return true;
    }

    void Stop() override
    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        running_ = false;
        /* Join whenever a thread exists — INCLUDING one that
           already self-terminated (COM init / unsupported
           format set running_=false inside ThreadMain), or
           the joinable std::thread would hit terminate in
           the destructor. */
        if(thread_.joinable())
            thread_.join();
        bus_ = nullptr;
        SetStatus("audio: off");
    }

    bool Running() const override { return running_.load(); }

    std::string Status() const override
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        return status_;
    }

    void SetPaused(bool paused) override
    {
        paused_.store(paused);
    }

    void SetSensitivity(float s) override
    {
        sens_.store(s);
    }

private:
    /* Cheap pre-flight under lifecycle_mu_: open and close
       the same session setup Pump() uses. Only conditions
       visible at open time are checked — mix-format support
       still needs a real buffer and stays an async status. */
    bool Probe(std::string& err)
    {
        const HRESULT hr_com =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninit = SUCCEEDED(hr_com); /* S_OK/S_FALSE */
        if(FAILED(hr_com) && hr_com != RPC_E_CHANGED_MODE)
        {
            err = "audio: COM init failed";
            return false;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice>           device;
        ComPtr<IAudioClient>        client;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX*               wfx = nullptr;

        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if(FAILED(hr))
        {
            err = "audio: no device enumerator";
        }
        else
        {
            hr = enumerator->GetDefaultAudioEndpoint(
                eRender, eConsole, &device);
            if(FAILED(hr))
            {
                err = "audio: no default output device";
            }
            else
            {
                hr = device->Activate(__uuidof(IAudioClient),
                                      CLSCTX_ALL, nullptr,
                                      &client);
                if(SUCCEEDED(hr))
                    hr = client->GetMixFormat(&wfx);
                if(SUCCEEDED(hr))
                    hr = client->Initialize(
                        AUDCLNT_SHAREMODE_SHARED,
                        AUDCLNT_STREAMFLAGS_LOOPBACK,
                        0, 0, wfx, nullptr);
                if(SUCCEEDED(hr))
                    hr = client->GetService(
                        IID_PPV_ARGS(&capture));
                if(FAILED(hr) || capture == nullptr)
                    err = "audio: loopback init failed";
            }
        }
        if(wfx != nullptr)
            CoTaskMemFree(wfx);
        if(uninit)
            CoUninitialize();
        return err.empty();
    }

    void SetStatus(const std::string& s)
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        status_ = s;
    }

    void ThreadMain()
    {
        const HRESULT hr =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if(FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        {
            SetStatus("audio: COM init failed");
            running_ = false;
            return;
        }
        while(running_.load())
        {
            if(!Pump())
                break;
            /* Session ended (device change or error) — brief
               backoff, then reopen. */
            for(int i = 0; i < 5 && running_.load(); i++)
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
        }
        if(bus_ != nullptr)
            bus_->SetAudioLevel(0.0f);
        CoUninitialize();
    }

    bool Pump()
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice>           device;
        ComPtr<IAudioClient>        client;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX*               wfx = nullptr;
        DefaultDeviceNotifier       notifier(&reinit_);

        HRESULT hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if(FAILED(hr))
        {
            SetStatus("audio: no device enumerator");
            return running_.load();
        }
        enumerator->RegisterEndpointNotificationCallback(&notifier);

        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                 &device);
        if(FAILED(hr))
        {
            enumerator->UnregisterEndpointNotificationCallback(
                &notifier);
            SetStatus("audio: no default output device");
            return running_.load();
        }
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                              nullptr, &client);
        if(SUCCEEDED(hr))
            hr = client->GetMixFormat(&wfx);
        if(SUCCEEDED(hr))
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    0, 0, wfx, nullptr);
        if(SUCCEEDED(hr))
            hr = client->GetService(IID_PPV_ARGS(&capture));
        if(FAILED(hr) || capture == nullptr)
        {
            enumerator->UnregisterEndpointNotificationCallback(
                &notifier);
            if(wfx != nullptr)
                CoTaskMemFree(wfx);
            SetStatus("audio: loopback init failed");
            return running_.load();
        }
        client->Start();
        SetStatus("audio: listening");
        reinit_ = false;

        const double rate = (double)wfx->nSamplesPerSec;
        while(running_.load() && !reinit_.load())
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
            UINT32 packet = 0;
            hr = capture->GetNextPacketSize(&packet);
            while(SUCCEEDED(hr) && packet > 0 &&
                  running_.load() && !reinit_.load())
            {
                BYTE*   data   = nullptr;
                UINT32  frames = 0;
                DWORD   flags  = 0;
                hr = capture->GetBuffer(&data, &frames, &flags,
                                        nullptr, nullptr);
                if(FAILED(hr))
                    break;
                if(paused_.load())
                {
                    /* display-off/suspend: drain so no stale
                       backlog bursts through on resume, feed
                       nothing */
                    capture->ReleaseBuffer(frames);
                }
                else
                {
                    float energy = 0.0f;
                    const bool ok =
                        ChunkEnergy(data, frames, wfx, flags,
                                    energy);
                    capture->ReleaseBuffer(frames);
                    if(!ok)
                    {
                        SetStatus("audio: unsupported mix format");
                        running_ = false;
                        break;
                    }
                    onset_.SetSensitivity(sens_.load());
                    const float hit =
                        onset_.Feed(energy, frames / rate);
                    if(bus_ != nullptr)
                    {
                        bus_->SetAudioLevel(onset_.Level());
                        if(hit > 0.0f)
                            /* Position-less event — the ripple
                               layer's origin decides where
                               rings spawn. */
                            bus_->PushEvent("audio", hit);
                    }
                }
                hr = capture->GetNextPacketSize(&packet);
            }
            if(FAILED(hr))
                break;  /* e.g. AUDCLNT_E_DEVICE_INVALIDATED */
        }

        client->Stop();
        enumerator->UnregisterEndpointNotificationCallback(
            &notifier);
        if(wfx != nullptr)
            CoTaskMemFree(wfx);
        if(!running_.load())
            SetStatus("audio: off");
        else
            SetStatus("audio: reinitializing");
        return running_.load();
    }

    studio::InputBus*    bus_ = nullptr;
    studio::OnsetDetect  onset_;
    std::mutex           lifecycle_mu_; /* Start/Stop body */
    std::thread          thread_;
    std::atomic<bool>    running_ { false };
    std::atomic<bool>    reinit_  { false };
    std::atomic<bool>    paused_  { false };
    std::atomic<float>   sens_    { 1.0f };
    mutable std::mutex   status_mu_;
    std::string          status_  = "audio: off";
};

} /* namespace */

std::unique_ptr<IReactiveSource> MakeAudioLoopbackSource()
{
    return std::make_unique<WinAudioLoopback>();
}

} /* namespace platform */
} /* namespace openrgb2 */

#else /* unsupported platform — report honestly, never crash */

namespace openrgb2
{
namespace platform
{

namespace
{
class StubAudio final : public IReactiveSource
{
public:
    bool Start(studio::InputBus*) override
    {
        status_ = "audio: unsupported platform";
        return false;
    }
    void        Stop()    override               {}
    bool        Running() const override         { return false; }
    std::string Status() const override          { return status_; }
private:
    std::string status_ = "audio: unsupported platform";
};
} /* namespace */

std::unique_ptr<IReactiveSource> MakeAudioLoopbackSource()
{
    return std::make_unique<StubAudio>();
}

} /* namespace platform */
} /* namespace openrgb2 */

#endif
