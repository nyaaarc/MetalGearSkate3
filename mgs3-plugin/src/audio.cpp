#include "audio.hpp"

#include <cmath>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "log.hpp"

#pragma comment(lib, "ole32.lib")

namespace audio {
namespace {

using RenderFn = int(__cdecl*)(const MgskSession*, float*, uint32_t);
using StartFn = int(__cdecl*)(MgskSession*, const char*);
using RunningFn = int(__cdecl*)(const MgskSession*);
using ErrorFn = const char*(__cdecl*)();

RenderFn g_render = nullptr;
StartFn g_start = nullptr;
RunningFn g_running = nullptr;
ErrorFn g_error = nullptr;

std::atomic<MgskSession*> g_session{nullptr};
std::atomic<bool> g_playing{false};
std::atomic<bool> g_stop{false};
std::atomic<float> g_volume{1.f};
std::atomic<uint64_t> g_frames{0}, g_underruns{0};
std::atomic<bool> g_open{false};
HANDLE g_thread = nullptr;

// The device buffer: 40 ms keeps a 60 Hz engine's posts close to their sound and survives a
// late wake-up.
constexpr REFERENCE_TIME kBufferDuration = 400000;  // 100 ns units
constexpr DWORD kRate = 48000;

template <class T>
void release(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

// One open device and its render loop; returns when stopping or the device goes away.
void play_device() {
  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDevice* device = nullptr;
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator));
  if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
  if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
  WAVEFORMATEXTENSIBLE format{};
  format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  format.Format.nChannels = 2;
  format.Format.nSamplesPerSec = kRate;
  format.Format.wBitsPerSample = 32;
  format.Format.nBlockAlign = 8;
  format.Format.nAvgBytesPerSec = kRate * 8;
  format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  format.Samples.wValidBitsPerSample = 32;
  format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  // Shared mode at our own rate: Windows resamples to the device's mix format.
  if (SUCCEEDED(hr)) {
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                            kBufferDuration, 0, &format.Format, nullptr);
  }
  if (SUCCEEDED(hr)) hr = client->SetEventHandle(event);
  UINT32 size = 0;
  if (SUCCEEDED(hr)) hr = client->GetBufferSize(&size);
  if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
  if (SUCCEEDED(hr)) hr = client->Start();
  if (FAILED(hr)) {
    log("audio: no output device (0x%08lX); the skater is silent", static_cast<unsigned long>(hr));
  } else {
    log("audio: playing on the default device, 48 kHz stereo, %u-frame buffer", size);
    g_open = true;
    bool wrote = false;
    while (!g_stop) {
      WaitForSingleObject(event, 200);
      UINT32 padding = 0;
      if (FAILED(hr = client->GetCurrentPadding(&padding))) break;  // device gone: reopen
      const UINT32 frames = size - padding;
      if (frames == 0) continue;
      if (padding == 0 && wrote && g_playing) g_underruns.fetch_add(1, std::memory_order_relaxed);
      BYTE* data = nullptr;
      if (FAILED(hr = render->GetBuffer(frames, &data))) break;
      MgskSession* session = g_session.load();
      DWORD flags = 0;
      if (g_playing && session && g_render(session, reinterpret_cast<float*>(data), frames) == 0) {
        const float volume = g_volume.load(std::memory_order_relaxed);
        if (volume != 1.f) {
          // Above 1 the engine's mix is boosted; a soft knee above 0.8 keeps peaks from clipping.
          float* f = reinterpret_cast<float*>(data);
          for (UINT32 i = 0; i < frames * 2; ++i) {
            float x = f[i] * volume;
            const float a = std::fabs(x);
            if (a > 0.8f) x = std::copysign(0.8f + 0.2f * std::tanh((a - 0.8f) / 0.2f), x);
            f[i] = x;
          }
        }
      } else {
        flags = AUDCLNT_BUFFERFLAGS_SILENT;
      }
      render->ReleaseBuffer(frames, flags);
      g_frames.fetch_add(frames, std::memory_order_relaxed);
      wrote = true;
    }
    client->Stop();
    g_open = false;
    if (!g_stop) log("audio: output stopped (0x%08lX); reopening", static_cast<unsigned long>(hr));
  }
  release(render);
  release(client);
  release(device);
  release(enumerator);
  CloseHandle(event);
}

DWORD WINAPI run(LPVOID) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  while (!g_stop) {
    play_device();
    // A failed or lost device: try again shortly (headphones plugged in, device switched).
    for (int i = 0; i < 20 && !g_stop; ++i) Sleep(100);
  }
  CoUninitialize();
  return 0;
}

}  // namespace

void set_volume(float volume) { g_volume = volume < 0.f ? 0.f : (volume > kMaxVolume ? kMaxVolume : volume); }

float volume() { return g_volume.load(); }

bool start(HMODULE ffi, float volume) {
  g_render = reinterpret_cast<RenderFn>(GetProcAddress(ffi, "mgsk_audio_render"));
  g_start = reinterpret_cast<StartFn>(GetProcAddress(ffi, "mgsk_audio_start"));
  g_running = reinterpret_cast<RunningFn>(GetProcAddress(ffi, "mgsk_audio_running"));
  g_error = reinterpret_cast<ErrorFn>(GetProcAddress(ffi, "mgsk_last_error"));
  if (!g_render || !g_start || !g_running) {
    log("audio: skate_ffi.dll has no audio exports; the skater is silent");
    return false;
  }
  set_volume(volume);
  g_stop = false;
  g_thread = CreateThread(nullptr, 0, run, nullptr, 0, nullptr);
  return g_thread != nullptr;
}

// Starting a session's sound reads and decodes its banks (~20 s): done here, off the skate
// thread, which keeps skating (silently) meanwhile. One at a time; stop() joins it.
std::thread g_loader;
std::string g_assets;

bool use_session(MgskSession* session, const char* assets_dir) {
  if (!g_start) return false;
  if (!session || g_running(session)) {
    g_session = session;
    return true;
  }
  if (g_loader.joinable()) g_loader.join();
  g_assets = assets_dir;
  g_session = nullptr;  // silent until this session's sound is up
  g_loader = std::thread([session] {
    const DWORD t0 = GetTickCount();
    const int banks = g_start(session, g_assets.c_str());
    if (banks < 0) {
      log("audio: the skater's sound did not start: %s (run setup's audio export)", g_error ? g_error() : "?");
      return;
    }
    log("audio: Skate 3 audio runtime on (%d banks) in %lu ms", banks, GetTickCount() - t0);
    MgskSession* expected = nullptr;
    g_session.compare_exchange_strong(expected, session);  // unless another session took over
  });
  return true;
}

void set_playing(bool playing) { g_playing = playing; }

void stop() {
  g_stop = true;
  if (g_loader.joinable()) g_loader.join();
  if (g_thread) {
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    g_thread = nullptr;
  }
  g_session = nullptr;
  g_playing = false;
}

Status status() {
  Status s;
  s.running = g_open;
  s.frames = g_frames.load();
  s.underruns = g_underruns.load();
  return s;
}

}  // namespace audio
